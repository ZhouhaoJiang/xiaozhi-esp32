#include "pomodoro_manager.h"
#include "sdcard_manager.h"
#include <esp_log.h>
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include <esp_ae_rate_cvt.h>
#include "board.h"
#include "display/display.h"
#include "application.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// ===== 公开接口 =====

bool PomodoroManager::start(int minutes, bool white_noise) {
    // 如果已经在运行，先停止
    if (state_.load() != IDLE) {
        stop();
        vTaskDelay(pdMS_TO_TICKS(500));  // 等待旧任务清理
    }

    minutes_ = minutes;
    play_white_noise_ = white_noise;

    int total_secs = minutes * 60;
    stop_requested_.store(false);
    noise_stop_requested_.store(false);
    remaining_seconds_.store(total_secs);
    total_seconds_.store(total_secs);
    state_.store(COUNTING);

    // 启动倒计时主任务
    xTaskCreatePinnedToCore(PomodoroTask, "pomodoro_task", 4 * 1024, this, 2, &pomodoro_task_handle_, 0);

    // 启动白噪音播放
    if (white_noise && SdcardManager::getInstance().isMounted()) {
        auto files = scanWhiteNoiseFiles();
        if (!files.empty()) {
            // 白噪音任务用较低优先级，避免影响 UI 与语音主流程
            xTaskCreatePinnedToCore(WhiteNoiseTask, "white_noise", 8 * 1024, this, 1, &noise_task_handle_, 0);
        } else {
            ESP_LOGW(TAG, "SD 卡 %s 目录下没有找到 MP3 文件，跳过白噪音", WHITE_NOISE_DIR);
        }
    }

    ESP_LOGI(TAG, "番茄钟已启动: %d 分钟倒计时, 白噪音=%s", minutes, white_noise ? "开" : "关");
    return true;
}

void PomodoroManager::stop() {
    if (state_.load() == IDLE) return;

    stop_requested_.store(true);
    stopWhiteNoise();
    state_.store(IDLE);
    remaining_seconds_.store(0);
    total_seconds_.store(0);

    // 等待任务退出
    if (pomodoro_task_handle_) {
        int wait_count = 0;
        while (pomodoro_task_handle_ && wait_count < 20) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count++;
        }
    }

    ESP_LOGI(TAG, "番茄钟已停止");
}

void PomodoroManager::togglePause() {
    State current = state_.load();
    if (current == COUNTING) {
        state_.store(PAUSED);
        stopWhiteNoise();
        ESP_LOGI(TAG, "番茄钟已暂停");
    } else if (current == PAUSED) {
        // 恢复倒计时，重新启动白噪音
        state_.store(COUNTING);
        if (play_white_noise_ && SdcardManager::getInstance().isMounted()) {
            noise_stop_requested_.store(false);
            auto files = scanWhiteNoiseFiles();
            if (!files.empty() && noise_task_handle_ == nullptr) {
                // 白噪音任务用较低优先级，避免影响 UI 与语音主流程
                xTaskCreatePinnedToCore(WhiteNoiseTask, "white_noise", 8 * 1024, this, 1, &noise_task_handle_, 0);
            }
        }
        ESP_LOGI(TAG, "番茄钟已恢复");
    }
}

std::string PomodoroManager::getStateText() const {
    switch (state_.load()) {
        case IDLE:     return "空闲";
        case COUNTING: return "倒计时中";
        case PAUSED:   return "已暂停";
        default:       return "未知";
    }
}

std::string PomodoroManager::getRemainingTimeStr() const {
    int secs = remaining_seconds_.load();
    int min = secs / 60;
    int sec = secs % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", min, sec);
    return std::string(buf);
}

PomodoroManager::~PomodoroManager() {
    stop();
}

// ===== 内部方法 =====

std::vector<std::string> PomodoroManager::scanWhiteNoiseFiles() {
    return SdcardManager::getInstance().listFiles(WHITE_NOISE_DIR, ".mp3");
}

void PomodoroManager::stopWhiteNoise() {
    noise_stop_requested_.store(true);
    int wait_count = 0;
    while (noise_task_handle_ && wait_count < 30) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }
}

// ===== 倒计时主任务（纯倒计时，到 0 就结束） =====

void PomodoroManager::PomodoroTask(void* arg) {
    auto* self = static_cast<PomodoroManager*>(arg);

    ESP_LOGI(TAG, "倒计时任务启动，共 %d 秒", self->remaining_seconds_.load());

    while (!self->stop_requested_.load()) {
        State current = self->state_.load();

        if (current == PAUSED) {
            // 暂停状态，不倒计时
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (current == IDLE) {
            break;
        }

        // 每秒倒计时
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (self->stop_requested_.load()) break;
        if (self->state_.load() == PAUSED) continue;

        int remaining = self->remaining_seconds_.load();
        if (remaining > 0) {
            self->remaining_seconds_.store(remaining - 1);
        }

        // 倒计时结束
        if (remaining <= 1) {
            ESP_LOGI(TAG, "倒计时结束！");

            // 停止白噪音
            self->stopWhiteNoise();

            // 屏幕显示提醒
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->SetChatMessage("system", "时间到！倒计时结束~");
            }

            // 回到空闲
            self->state_.store(IDLE);
            self->remaining_seconds_.store(0);
            self->total_seconds_.store(0);
            break;
        }
    }

    self->pomodoro_task_handle_ = nullptr;
    ESP_LOGI(TAG, "倒计时任务退出");
    vTaskDelete(nullptr);
}

// ===== 白噪音播放任务 =====

void PomodoroManager::WhiteNoiseTask(void* arg) {
    auto* self = static_cast<PomodoroManager*>(arg);
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    auto& app = Application::GetInstance();

    ESP_LOGI(TAG, "白噪音播放任务启动");

    // 扫描可用的白噪音文件
    auto files = self->scanWhiteNoiseFiles();
    if (files.empty()) {
        ESP_LOGW(TAG, "没有找到白噪音文件");
        self->noise_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // 选第一个文件（如果有多个可以后续加随机）
    std::string file_path = files[0];
    ESP_LOGI(TAG, "播放白噪音: %s", file_path.c_str());

    // 解码器相关变量
    constexpr int kReadBufSize = 2048;
    int out_buf_size = 8192;
    uint8_t* in_buf = nullptr;
    uint8_t* out_buf = nullptr;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    bool decoder_registered = false;
    bool noise_output_enabled = false;

    in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadBufSize, MALLOC_CAP_8BIT));
    out_buf = static_cast<uint8_t*>(heap_caps_malloc(out_buf_size, MALLOC_CAP_8BIT));
    if (!in_buf || !out_buf) {
        ESP_LOGE(TAG, "内存不足，无法播放白噪音");
        goto cleanup;
    }

    // 循环播放（文件播完后重新打开）
    while (!self->noise_stop_requested_.load()) {
        FILE* fp = fopen(file_path.c_str(), "rb");
        if (!fp) {
            ESP_LOGE(TAG, "无法打开白噪音文件: %s", file_path.c_str());
            break;
        }

        // 注册并打开 MP3 解码器
        esp_audio_err_t ret = esp_audio_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "注册默认解码器失败: %d", ret);
            fclose(fp);
            break;
        }
        ret = esp_audio_simple_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "注册简单解码器失败: %d", ret);
            esp_audio_dec_unregister_default();
            fclose(fp);
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
        if (ret != ESP_AUDIO_ERR_OK || !decoder) {
            ESP_LOGE(TAG, "打开 MP3 解码器失败: %d", ret);
            fclose(fp);
            break;
        }

        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
            noise_output_enabled = true;
        }

        bool info_ready = false;
        int stream_sample_rate = codec->output_sample_rate();
        int stream_channels = 1;

        // 读取并解码文件
        while (!self->noise_stop_requested_.load()) {
            // AI 语音会话期间让出音频通道，避免与白噪音抢占导致卡顿
            DeviceState ds = app.GetDeviceState();
            // 语音链路占用期间（连接/聆听/播报）都让出音频输出，
            // 避免白噪音与 ASR/TTS 反复抢占导致 I2S 通道频繁重开。
            const bool in_voice_session = (ds == kDeviceStateConnecting ||
                                           ds == kDeviceStateListening ||
                                           ds == kDeviceStateSpeaking);
            if (in_voice_session) {
                if (noise_output_enabled && codec->output_enabled()) {
                    codec->EnableOutput(false);
                    noise_output_enabled = false;
                }
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }
            if (!noise_output_enabled && !codec->output_enabled()) {
                codec->EnableOutput(true);
                noise_output_enabled = true;
            }

            int read_bytes = fread(in_buf, 1, kReadBufSize, fp);
            if (read_bytes <= 0) {
                // 文件读完，跳出内层循环准备重新播放
                ESP_LOGI(TAG, "白噪音文件播放完一轮，准备循环");
                break;
            }

            esp_audio_simple_dec_raw_t raw = {
                .buffer = in_buf,
                .len = static_cast<uint32_t>(read_bytes),
                .eos = (read_bytes < kReadBufSize),
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };

            while (raw.len > 0 && !self->noise_stop_requested_.load()) {
                esp_audio_simple_dec_out_t out = {
                    .buffer = out_buf,
                    .len = static_cast<uint32_t>(out_buf_size),
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                ret = esp_audio_simple_dec_process(decoder, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    auto* new_buf = static_cast<uint8_t*>(heap_caps_realloc(out_buf, out.needed_size, MALLOC_CAP_8BIT));
                    if (!new_buf) {
                        ESP_LOGE(TAG, "扩容解码缓冲区失败");
                        goto file_done;
                    }
                    out_buf = new_buf;
                    out_buf_size = static_cast<int>(out.needed_size);
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    // 解码错误，跳过这段数据
                    break;
                }

                // 获取解码信息（只需一次）
                if (!info_ready && out.decoded_size > 0) {
                    esp_audio_simple_dec_info_t dec_info = {};
                    if (esp_audio_simple_dec_get_info(decoder, &dec_info) == ESP_AUDIO_ERR_OK) {
                        stream_sample_rate = static_cast<int>(dec_info.sample_rate);
                        stream_channels = std::max(1, static_cast<int>(dec_info.channel));
                        info_ready = true;
                        ESP_LOGI(TAG, "白噪音解码信息: sample_rate=%d channel=%d", stream_sample_rate, stream_channels);
                    }
                }

                if (out.decoded_size > 0) {
                    std::vector<int16_t> pcm(out.decoded_size / sizeof(int16_t));
                    memcpy(pcm.data(), out.buffer, out.decoded_size);

                    // 声道转换
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

                    // 重采样
                    if (stream_sample_rate != codec->output_sample_rate()) {
                        if (!resampler) {
                            esp_ae_rate_cvt_cfg_t cfg = {
                                .src_rate = static_cast<uint32_t>(stream_sample_rate),
                                .dest_rate = static_cast<uint32_t>(codec->output_sample_rate()),
                                .channel = static_cast<uint8_t>(codec->output_channels()),
                                .bits_per_sample = ESP_AUDIO_BIT16,
                                .complexity = 2,
                                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                            };
                            esp_ae_rate_cvt_open(&cfg, &resampler);
                        }
                        if (resampler) {
                            uint32_t in_samples = static_cast<uint32_t>(pcm.size() / codec->output_channels());
                            uint32_t out_samples = 0;
                            esp_ae_rate_cvt_get_max_out_sample_num(resampler, in_samples, &out_samples);
                            std::vector<int16_t> resampled(out_samples * codec->output_channels());
                            uint32_t actual_out = out_samples;
                            esp_ae_rate_cvt_process(
                                resampler,
                                reinterpret_cast<esp_ae_sample_t>(pcm.data()),
                                in_samples,
                                reinterpret_cast<esp_ae_sample_t>(resampled.data()),
                                &actual_out);
                            resampled.resize(actual_out * codec->output_channels());
                            pcm = std::move(resampled);
                        }
                    }

                    if (!pcm.empty()) {
                        if (!codec->output_enabled()) {
                            codec->EnableOutput(true);
                            noise_output_enabled = true;
                        }
                        codec->OutputData(pcm);
                    }
                }

                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
            }
        }

file_done:
        fclose(fp);

        // 关闭解码器（每轮循环重新打开，确保状态干净）
        if (decoder) {
            esp_audio_simple_dec_close(decoder);
            decoder = nullptr;
        }
        if (decoder_registered) {
            esp_audio_simple_dec_unregister_default();
            esp_audio_dec_unregister_default();
            decoder_registered = false;
        }
        if (resampler) {
            esp_ae_rate_cvt_close(resampler);
            resampler = nullptr;
        }

        // 循环间隔（避免文件读完后立即重开导致 CPU 占用过高）
        vTaskDelay(pdMS_TO_TICKS(100));
    }

cleanup:
    if (decoder) {
        esp_audio_simple_dec_close(decoder);
    }
    if (decoder_registered) {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
    }
    if (resampler) {
        esp_ae_rate_cvt_close(resampler);
    }
    if (in_buf) heap_caps_free(in_buf);
    if (out_buf) heap_caps_free(out_buf);
    if (noise_output_enabled && codec->output_enabled()) {
        codec->EnableOutput(false);
    }

    self->noise_task_handle_ = nullptr;
    ESP_LOGI(TAG, "白噪音播放任务退出");
    vTaskDelete(nullptr);
}
