#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <algorithm>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <esp_heap_caps.h>
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_ae_rate_cvt.h"

#define TAG "Application"

namespace {
struct MusicPlaybackTaskArgs {
    Application* app = nullptr;
    std::string url;
    std::string title;
    std::string artist;
    std::string lyric_url;
};
}  // namespace


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Play the success sound to indicate the device is ready
    audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        // 如果正在播放音乐（HTTP 流），不要切回省电模式，
        // 否则会覆盖 PlayMusicFromUrl 里设的高性能模式导致卡顿
        if (!music_playing_.load()) {
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        }
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    // 用户开始交互时优先打断本地音乐播放，确保语音链路可立即接管音频设备。
    if (IsMusicPlaying()) {
        StopMusicPlayback();
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    // 用户按下开始监听时，如果正在播歌，先立即停止。
    if (IsMusicPlaying()) {
        StopMusicPlayback();
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (IsMusicPlaying()) {
        StopMusicPlayback();
    }

    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // 本地音乐播放期间保持歌词/曲目信息，避免被空闲态重绘清掉。
            if (IsMusicPlaying()) {
                display->SetStatus("音乐播放");
            } else {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ClearChatMessages();  // Clear messages first
                display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }

            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    StopMusicPlayback();

    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

bool Application::PlayMusicFromUrl(const std::string& url, const std::string& title, const std::string& artist,
                                   const std::string& lyric, const std::string& lyric_url) {
    if (url.empty()) {
        return false;
    }

    // 防重播保护：同一 URL 在播放结束后 15 秒内不允许重复启动
    // 防止大模型"播放结束 -> 回答用户 -> 又调一次 play_url"的死循环
    {
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        if (!last_played_url_.empty() && last_played_url_ == url && last_play_finished_ms_ > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t elapsed = now_ms - last_play_finished_ms_;
            if (elapsed < 15000) {
                ESP_LOGW(TAG, "防重播：同一 URL 在 %lld ms 前刚播完，跳过", elapsed);
                return false;
            }
        }
    }

    // 先停止旧播放，避免多个任务同时争用音频输出。
    StopMusicPlayback(false);

    // 音乐播放前先退出当前语音会话，避免服务端音频与本地音乐混播。
    if (GetDeviceState() == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    SetDeviceState(kDeviceStateIdle);
    audio_service_.ResetDecoder();
    audio_service_.SetExternalPlaybackActive(true);

    // 音乐播放走 HTTP 流，不经过 AudioChannel，WiFi 不会自动切高性能
    // 这里手动关闭省电，避免 MAX_MODEM 休眠导致下载卡顿
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    {
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        stop_music_playback_.store(false);
        music_playing_.store(true);
        music_progress_ms_.store(0);
        music_total_ms_.store(0);
        current_music_title_ = title;
        current_music_url_ = url;
    }

    auto display_title = title.empty() ? "未知歌曲" : title;
    auto display_artist = artist.empty() ? "未知歌手" : artist;
    Schedule([display_title, display_artist]() {
        auto display = Board::GetInstance().GetDisplay();
        // 触发音乐播放时，优先切到音乐页，避免歌词继续显示在首页 AI 卡片
        display->SwitchToMusicPage();
        display->SetStatus("音乐播放");
        display->SetChatMessage("system", "音乐播放中~");
        display->SetMusicInfo(display_title.c_str(), display_artist.c_str());
        display->SetMusicProgress(0, 1);
        display->SetMusicLyric("");
    });
    if (!lyric.empty()) {
        UpdateMusicLyric(lyric);
    }

    auto* args = new MusicPlaybackTaskArgs();
    args->app = this;
    args->url = url;
    args->title = display_title;
    args->artist = display_artist;
    // 优先使用直接歌词文本；如果为空则把 lyric_url 传给播放任务，让它自行拉取
    args->lyric_url = lyric.empty() ? lyric_url : "";

    BaseType_t created = xTaskCreate([](void* arg) {
        std::unique_ptr<MusicPlaybackTaskArgs> task_args(static_cast<MusicPlaybackTaskArgs*>(arg));
        task_args->app->MusicPlaybackTask(std::move(task_args->url), std::move(task_args->title),
                                          std::move(task_args->artist), std::move(task_args->lyric_url));
        vTaskDelete(nullptr);
    }, "music_playback", 12288, args, 3, &music_playback_task_handle_);

    if (created != pdPASS) {
        delete args;
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        music_playing_.store(false);
        music_playback_task_handle_ = nullptr;
        current_music_title_.clear();
        return false;
    }

    return true;
}

void Application::MusicPlaybackTask(std::string url, std::string title, std::string artist, std::string lyric_url) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();
    auto network = board.GetNetwork();
    ESP_LOGI(TAG, "开始播放音乐: %s - %s", title.c_str(), artist.c_str());

    // LRC 歌词结构：(时间戳毫秒, 纯文本)
    struct LyricLine {
        uint32_t time_ms;
        std::string text;
    };
    std::vector<LyricLine> lyrics;
    size_t current_lyric_idx = 0;
    size_t last_displayed_idx = SIZE_MAX;  // 避免重复刷新
    uint32_t lyric_total_ms = 0;

    // 如果有歌词 URL，拉取并解析 LRC 格式
    if (!lyric_url.empty()) {
        ESP_LOGI(TAG, "正在拉取歌词: %s", lyric_url.c_str());
        auto lyric_http = network->CreateHttp(4);  // 用不同的 connect_id，避免和音乐流(3)的 TLS 连接冲突
        lyric_http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
        if (lyric_http->Open("GET", lyric_url)) {
            if (lyric_http->GetStatusCode() == 200) {
                std::string lyric_text = lyric_http->ReadAll();
                if (!lyric_text.empty()) {
                    ESP_LOGI(TAG, "歌词拉取成功，长度=%d bytes", (int)lyric_text.size());

                    // 解析 LRC 歌词：逐行提取 [mm:ss.xx] 时间戳和纯文本
                    size_t pos = 0;
                    while (pos < lyric_text.size()) {
                        // 找到行尾
                        size_t line_end = lyric_text.find('\n', pos);
                        if (line_end == std::string::npos) line_end = lyric_text.size();
                        std::string line = lyric_text.substr(pos, line_end - pos);
                        pos = line_end + 1;

                        // 去掉行尾 \r
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.empty()) continue;

                        // 提取所有时间标签 [mm:ss.xx] 或 [mm:ss.xxx]
                        std::string pure_text;
                        std::vector<uint32_t> timestamps;
                        size_t i = 0;
                        while (i < line.size() && line[i] == '[') {
                            size_t close = line.find(']', i);
                            if (close == std::string::npos) break;
                            std::string tag = line.substr(i + 1, close - i - 1);
                            i = close + 1;

                            // 尝试解析时间 mm:ss.xx
                            int mm = 0, ss = 0, ms = 0;
                            if (sscanf(tag.c_str(), "%d:%d.%d", &mm, &ss, &ms) >= 2) {
                                // 处理毫秒位数：.27 → 270ms, .270 → 270ms, .5 → 500ms
                                if (ms < 10 && tag.find('.') != std::string::npos) {
                                    size_t dot = tag.find('.');
                                    size_t ms_digits = tag.size() - dot - 1;
                                    if (ms_digits == 1) ms *= 100;
                                    else if (ms_digits == 2) ms *= 10;
                                }
                                timestamps.push_back(mm * 60000 + ss * 1000 + ms);
                            }
                            // 非时间标签（如 [ti:标题]）直接跳过
                        }
                        pure_text = line.substr(i);

                        // 跳过空歌词和元信息行
                        if (pure_text.empty()) continue;

                        // 每个时间戳对应同一句歌词
                        for (uint32_t ts : timestamps) {
                            lyrics.push_back({ts, pure_text});
                        }
                    }

                    // 按时间排序
                    std::sort(lyrics.begin(), lyrics.end(),
                              [](const LyricLine& a, const LyricLine& b) { return a.time_ms < b.time_ms; });
                    if (!lyrics.empty()) {
                        lyric_total_ms = std::max<uint32_t>(lyrics.back().time_ms, 1);
                    }

                    ESP_LOGI(TAG, "LRC 歌词解析完成，共 %d 行有效歌词", (int)lyrics.size());
                } else {
                    ESP_LOGW(TAG, "歌词内容为空");
                }
            } else {
                ESP_LOGW(TAG, "歌词拉取失败，HTTP 状态码: %d", lyric_http->GetStatusCode());
            }
            lyric_http->Close();
        } else {
            ESP_LOGW(TAG, "歌词拉取失败：无法连接到歌词服务");
        }
    }
    constexpr int kReadBufSize = 2048;
    int out_buf_size = 8192;
    bool decoder_registered = false;
    bool playback_finished = false;
    bool playback_failed = false;
    bool notify_failed = false;
    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_ae_rate_cvt_handle_t music_resampler = nullptr;
    uint8_t* in_buf = nullptr;
    uint8_t* out_buf = nullptr;
    bool info_ready = false;
    int stream_sample_rate = codec->output_sample_rate();
    int stream_channels = codec->output_channels();
    int zero_read_count = 0;
    constexpr int kMaxZeroReads = 30;  // 约 3 秒，避免网络抖动导致误判 EOF。
    size_t total_read_bytes = 0;
    size_t total_output_samples = 0;

    auto http = network->CreateHttp(3);
    do {
        http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "播放音乐失败：无法打开 URL: %s", url.c_str());
            playback_failed = true;
            Schedule([display]() {
                // 音乐页可见时，必须把失败信息写到歌词区，避免用户看不到弹窗。
                display->SetMusicLyric("播放失败：链接无效或网络异常");
                display->SetMusicProgress(0, 1);
                display->ShowNotification("音乐播放失败：网络连接异常");
            });
            notify_failed = true;
            break;
        }
        if (http->GetStatusCode() != 200) {
            ESP_LOGE(TAG, "播放音乐失败：HTTP 状态码 %d", http->GetStatusCode());
            playback_failed = true;
            Schedule([display]() {
                // 资源不可用时，同步更新音乐页提示，避免只在通知区显示。
                display->SetMusicLyric("播放失败：资源不可用");
                display->SetMusicProgress(0, 1);
                display->ShowNotification("音乐播放失败：资源不可用");
            });
            notify_failed = true;
            break;
        }

        ret = esp_audio_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "注册默认解码器失败: %d", ret);
            playback_failed = true;
            break;
        }
        ret = esp_audio_simple_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "注册简单解码器失败: %d", ret);
            playback_failed = true;
            break;
        }
        decoder_registered = true;

        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = nullptr,
            .cfg_size = 0,
            .use_frame_dec = false,
        };
        ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
            ESP_LOGE(TAG, "打开 MP3 解码器失败: %d", ret);
            playback_failed = true;
            break;
        }

        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }

        in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadBufSize, MALLOC_CAP_8BIT));
        out_buf = static_cast<uint8_t*>(heap_caps_malloc(out_buf_size, MALLOC_CAP_8BIT));
        if (in_buf == nullptr || out_buf == nullptr) {
            ESP_LOGE(TAG, "内存不足，无法播放音乐");
            playback_failed = true;
            break;
        }

        while (!stop_music_playback_.load()) {
            int read_bytes = http->Read(reinterpret_cast<char*>(in_buf), kReadBufSize);
            if (read_bytes < 0) {
                ESP_LOGE(TAG, "读取音乐流失败: %d", read_bytes);
                playback_failed = true;
                break;
            }
            if (read_bytes == 0) {
                zero_read_count++;
                if (zero_read_count >= kMaxZeroReads) {
                    ESP_LOGI(TAG, "音乐流读取结束（连续空读 %d 次）", zero_read_count);
                    playback_finished = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            zero_read_count = 0;
            total_read_bytes += static_cast<size_t>(read_bytes);

            esp_audio_simple_dec_raw_t raw = {
                .buffer = in_buf,
                .len = static_cast<uint32_t>(read_bytes),
                .eos = read_bytes < kReadBufSize,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };

            while (raw.len > 0 && !stop_music_playback_.load()) {
                esp_audio_simple_dec_out_t out = {
                    .buffer = out_buf,
                    .len = static_cast<uint32_t>(out_buf_size),
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                ret = esp_audio_simple_dec_process(decoder, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    auto* new_buf = static_cast<uint8_t*>(heap_caps_realloc(out_buf, out.needed_size, MALLOC_CAP_8BIT));
                    if (new_buf == nullptr) {
                        ESP_LOGE(TAG, "扩容解码输出缓冲区失败");
                        playback_failed = true;
                        break;
                    }
                    out_buf = new_buf;
                    out_buf_size = static_cast<int>(out.needed_size);
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "MP3 解码失败，ret=%d", ret);
                    playback_failed = true;
                    break;
                }

                if (!info_ready && out.decoded_size > 0) {
                    esp_audio_simple_dec_info_t dec_info = {};
                    if (esp_audio_simple_dec_get_info(decoder, &dec_info) == ESP_AUDIO_ERR_OK) {
                        stream_sample_rate = static_cast<int>(dec_info.sample_rate);
                        stream_channels = std::max(1, static_cast<int>(dec_info.channel));
                        info_ready = true;
                        ESP_LOGI(TAG, "MP3 解码信息: sample_rate=%d channel=%d", stream_sample_rate, stream_channels);
                    }
                }

                if (out.decoded_size > 0) {
                    std::vector<int16_t> pcm(out.decoded_size / sizeof(int16_t));
                    memcpy(pcm.data(), out.buffer, out.decoded_size);

                    // 先做声道转换，统一到 codec 输出声道数。
                    if (stream_channels == 2 && codec->output_channels() == 1) {
                        std::vector<int16_t> mono(pcm.size() / 2);
                        for (size_t i = 0, j = 0; i < mono.size(); ++i, j += 2) {
                            mono[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[j]) + static_cast<int32_t>(pcm[j + 1])) / 2);
                        }
                        pcm = std::move(mono);
                    } else if (stream_channels == 1 && codec->output_channels() == 2) {
                        std::vector<int16_t> stereo(pcm.size() * 2);
                        for (size_t i = 0; i < pcm.size(); ++i) {
                            stereo[i * 2] = pcm[i];
                            stereo[i * 2 + 1] = pcm[i];
                        }
                        pcm = std::move(stereo);
                    }

                    // 采样率不一致时重采样，避免“变速/变调”问题。
                    if (stream_sample_rate != codec->output_sample_rate()) {
                        if (music_resampler == nullptr) {
                            esp_ae_rate_cvt_cfg_t cfg = {
                                .src_rate = static_cast<uint32_t>(stream_sample_rate),
                                .dest_rate = static_cast<uint32_t>(codec->output_sample_rate()),
                                .channel = static_cast<uint8_t>(codec->output_channels()),
                                .bits_per_sample = ESP_AUDIO_BIT16,
                                .complexity = 2,
                                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                            };
                            auto cvt_ret = esp_ae_rate_cvt_open(&cfg, &music_resampler);
                            if (music_resampler == nullptr) {
                                ESP_LOGE(TAG, "创建重采样器失败: %d", cvt_ret);
                                playback_failed = true;
                                break;
                            }
                        }
                        uint32_t in_samples = static_cast<uint32_t>(pcm.size() / codec->output_channels());
                        uint32_t out_samples = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(music_resampler, in_samples, &out_samples);
                        std::vector<int16_t> resampled(out_samples * codec->output_channels());
                        uint32_t actual_out = out_samples;
                        esp_ae_rate_cvt_process(
                            music_resampler,
                            reinterpret_cast<esp_ae_sample_t>(pcm.data()),
                            in_samples,
                            reinterpret_cast<esp_ae_sample_t>(resampled.data()),
                            &actual_out);
                        resampled.resize(actual_out * codec->output_channels());
                        pcm = std::move(resampled);
                    }

                    if (!pcm.empty()) {
                        total_output_samples += pcm.size();
                        // 音乐播放绕过了 AudioService，需要手动保持功放开启。
                        if (!codec->output_enabled()) {
                            codec->EnableOutput(true);
                        }
                        codec->OutputData(pcm);

                        // 计算当前播放位置（无论有没有歌词都需要）
                        uint32_t current_ms = static_cast<uint32_t>(
                            static_cast<uint64_t>(total_output_samples) * 1000
                            / (codec->output_sample_rate() * codec->output_channels()));

                        // 同步更新原子变量，供 MCP 工具查询当前进度
                        music_progress_ms_.store(current_ms);
                        if (lyric_total_ms > 0) {
                            music_total_ms_.store(lyric_total_ms);
                        }

                        // 更新进度条（无论有没有歌词都要更新）
                        // 没有歌词时 lyric_total_ms=0，用 0 让 SetMusicProgress 只显示播放时间
                        display->SetMusicProgress(current_ms, lyric_total_ms);

                        // 有歌词时同步歌词显示
                        if (!lyrics.empty()) {
                            // 向前搜索到当前时间对应的歌词行
                            while (current_lyric_idx + 1 < lyrics.size()
                                   && lyrics[current_lyric_idx + 1].time_ms <= current_ms) {
                                current_lyric_idx++;
                            }
                            // 歌词行变化时更新屏幕显示（传三行：上一句\n当前句\n下一句）
                            if (current_lyric_idx != last_displayed_idx) {
                                last_displayed_idx = current_lyric_idx;
                                std::string prev_line = (current_lyric_idx > 0) ? lyrics[current_lyric_idx - 1].text : "";
                                std::string curr_line = lyrics[current_lyric_idx].text;
                                std::string next_line = (current_lyric_idx + 1 < lyrics.size()) ? lyrics[current_lyric_idx + 1].text : "";
                                UpdateMusicLyric(prev_line + "\n" + curr_line + "\n" + next_line);
                            }
                        }
                    }
                }

                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
            }

            if (playback_failed) {
                break;
            }
        }
    } while (false);

    if (music_resampler != nullptr) {
        esp_ae_rate_cvt_close(music_resampler);
    }
    if (in_buf != nullptr) {
        heap_caps_free(in_buf);
    }
    if (out_buf != nullptr) {
        heap_caps_free(out_buf);
    }
    if (decoder != nullptr) {
        esp_audio_simple_dec_close(decoder);
    }
    if (decoder_registered) {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
    }
    if (http) {
        http->Close();
    }
    audio_service_.SetExternalPlaybackActive(false);

    bool stopped_by_user = stop_music_playback_.load();
    {
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        music_playing_.store(false);
        stop_music_playback_.store(false);
        music_playback_task_handle_ = nullptr;
        // 记录刚播完的 URL 和时间戳，用于防重播保护
        last_played_url_ = url;
        last_play_finished_ms_ = esp_timer_get_time() / 1000;

        // ── 自动保存播放进度到缓存（无需 AI 干预）──
        uint32_t final_progress = music_progress_ms_.load();
        uint32_t final_total = music_total_ms_.load();
        if (!url.empty() && final_progress > 0) {
            // 如果缓存满了，删掉最旧的（简单策略：删第一个）
            if (music_progress_cache_.size() >= kMaxProgressCacheSize) {
                music_progress_cache_.erase(music_progress_cache_.begin());
            }
            music_progress_cache_[url] = {
                current_music_title_,
                final_progress,
                final_total
            };
            ESP_LOGI(TAG, "自动保存播放进度: %s @ %lu/%lu ms",
                     current_music_title_.c_str(),
                     static_cast<unsigned long>(final_progress),
                     static_cast<unsigned long>(final_total));
        }

        current_music_title_.clear();
    }

    // 播放结束，恢复 WiFi 省电模式
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    if (stopped_by_user) {
        ESP_LOGI(TAG, "音乐播放已被用户中断，累计读取=%u bytes, 输出=%u samples",
                 static_cast<unsigned>(total_read_bytes),
                 static_cast<unsigned>(total_output_samples));
        return;
    }
    if (playback_failed && !notify_failed) {
        ESP_LOGW(TAG, "音乐播放失败，累计读取=%u bytes, 输出=%u samples",
                 static_cast<unsigned>(total_read_bytes),
                 static_cast<unsigned>(total_output_samples));
        Schedule([display]() {
            display->SetMusicLyric("播放失败：请稍后重试");
            display->SetMusicProgress(0, 1);
            display->ShowNotification("音乐播放失败");
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                display->SetStatus(Lang::Strings::STANDBY);
            }
        });
        return;
    }
    if (playback_finished) {
        ESP_LOGI(TAG, "音乐播放结束，累计读取=%u bytes, 输出=%u samples",
                 static_cast<unsigned>(total_read_bytes),
                 static_cast<unsigned>(total_output_samples));
        Schedule([display]() {
            // 清除歌词/聊天消息区域，恢复待命状态
            display->SetMusicLyric("");
            display->SetMusicProgress(0, 1);
            // 播放结束后主动重置 AI 文案，避免停留在上一条工具/提示文本
            display->SetChatMessage("system", "AI 待命");
            display->ShowNotification("音乐播放结束");
            // 自动返回天气首页，避免停留在空白音乐页
            display->SwitchToWeatherPage();
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                display->SetStatus(Lang::Strings::STANDBY);
            }
        });
    }
}

void Application::StopMusicPlayback(bool clear_lyric) {
    TaskHandle_t handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        handle = music_playback_task_handle_;
        if (handle == nullptr) {
            music_playing_.store(false);
            current_music_title_.clear();
        } else {
            stop_music_playback_.store(true);
        }
    }

    // 等待播放任务主动退出，避免直接删任务导致解码器/HTTP 资源泄漏。
    while (handle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(20));
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        handle = music_playback_task_handle_;
    }

    if (clear_lyric) {
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetMusicLyric("");
            display->SetMusicProgress(0, 1);
            if (GetDeviceState() == kDeviceStateIdle) {
                display->SetStatus(Lang::Strings::STANDBY);
            }
        });
    }
}

void Application::UpdateMusicLyric(const std::string& lyric) {
    if (!IsMusicPlaying()) {
        return;
    }
    Schedule([lyric]() {
        auto display = Board::GetInstance().GetDisplay();
        // 歌词刷新时确保音乐页可见，避免内容落在首页 AI 区域
        display->SwitchToMusicPage();
        display->SetMusicLyric(lyric.c_str());
    });
}

void Application::ResetProtocol() {
    StopMusicPlayback();

    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

