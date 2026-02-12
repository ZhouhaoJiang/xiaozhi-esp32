#ifndef __POMODORO_MANAGER_H__
#define __POMODORO_MANAGER_H__

#include <atomic>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 番茄钟管理器（单例）
//
// 功能：
//   1. 纯倒计时（用户指定分钟数，倒计时到 0 结束）
//   2. 从 SD 卡读取白噪音 MP3 循环播放
//   3. 倒计时结束后语音/屏幕提醒
//
// 白噪音文件放在 SD 卡的 /sdcard/white-noise/ 目录下，支持 .mp3 格式。
// 如果有多个文件，随机选一个播放并循环。
//
// 入口：通过 AI 语音调用 MCP 工具 self.pomodoro.start / stop / status
class PomodoroManager {
public:
    // 番茄钟状态（简化版：只有空闲、倒计时中、已暂停）
    enum State {
        IDLE = 0,       // 空闲（未启动）
        COUNTING,       // 倒计时中
        PAUSED,         // 已暂停
    };

    static PomodoroManager& getInstance() {
        static PomodoroManager instance;
        return instance;
    }

    // 开始倒计时（minutes: 倒计时分钟数, white_noise: 是否播放白噪音）
    bool start(int minutes = 25, bool white_noise = true);

    // 停止（停止倒计时 + 停止白噪音）
    void stop();

    // 暂停/恢复
    void togglePause();

    // 获取当前状态
    State getState() const { return state_.load(); }

    // 获取剩余秒数
    int getRemainingSeconds() const { return remaining_seconds_.load(); }

    // 获取总秒数
    int getTotalSeconds() const { return total_seconds_.load(); }

    // 获取设定的分钟数
    int getMinutes() const { return minutes_; }

    // 获取状态文字描述
    std::string getStateText() const;

    // 获取格式化的剩余时间 "MM:SS"
    std::string getRemainingTimeStr() const;

private:
    PomodoroManager() = default;
    ~PomodoroManager();

    PomodoroManager(const PomodoroManager&) = delete;
    PomodoroManager& operator=(const PomodoroManager&) = delete;

    // 倒计时主任务
    static void PomodoroTask(void* arg);

    // 白噪音播放任务（从 SD 卡读 MP3 循环播放）
    static void WhiteNoiseTask(void* arg);

    // 停止白噪音播放
    void stopWhiteNoise();

    // 扫描 SD 卡白噪音文件
    std::vector<std::string> scanWhiteNoiseFiles();

    static constexpr const char* TAG = "PomodoroManager";
    static constexpr const char* WHITE_NOISE_DIR = "/sdcard/white-noise";

    std::atomic<State> state_{IDLE};
    std::atomic<int> remaining_seconds_{0};
    std::atomic<int> total_seconds_{0};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> noise_stop_requested_{false};

    int minutes_ = 25;
    bool play_white_noise_ = true;

    TaskHandle_t pomodoro_task_handle_ = nullptr;
    TaskHandle_t noise_task_handle_ = nullptr;
};

#endif
