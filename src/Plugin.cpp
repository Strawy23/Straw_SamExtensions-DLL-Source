









#include "f4se/PluginAPI.h"
#include "f4se/PapyrusNativeFunctions.h"
#include "f4se/PapyrusVM.h"
#include "f4se/GameAPI.h"
#include "f4se/GameReferences.h"
#include "f4se/GameForms.h"
#include "f4se/GameThreads.h"
#include "f4se/NiNodes.h"
#include "f4se/NiObjects.h"
#include <Psapi.h>
#include <algorithm>
#include <cmath>
#include <unordered_set>

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
F4SETaskInterface* g_task = nullptr;

namespace
{
    constexpr UInt32 kPluginVersion = 86;
    constexpr int kSysWindowCompileAndRun = 1;
    constexpr std::size_t kMaxBatchCommands = 512;
    constexpr std::size_t kMaxCommandLength = 4095;
    constexpr bool kEnableFileLog = false;


    constexpr float kDegToRad = 0.01745329251994329577f;
    constexpr float kRadToDeg = 57.295779513082320876f;
    constexpr int kFaceTickMs = 50;
    constexpr int kObjectSpinTickMs = 20;
    constexpr int kPlayerDirectSpinTickMs = 20;
    constexpr int kSwitchDelayMs = 75;
    constexpr float kSpinSegmentDeg = 30.0f;
    constexpr int kDefaultMaxActiveSpinners = 8;
    constexpr int kHardMaxActiveSpinners = 32;

    struct SpinState
    {
        UInt32 targetId = 0;
        UInt32 playerId = 0;
        UInt32 key = 0;
        UInt64 generation = 0;
        int mode = 0;
        float speedDeg = 45.0f;
        float dir = -1.0f;
        std::atomic_bool active{ false };
        std::atomic_bool paused{ false };
        std::atomic_bool restoreQueued{ false };
        std::mutex lock;
        bool hasBaseAngle = false;
        float baseAngleRad = 0.0f;
        float phaseRad = 0.0f;
        bool stepAngle = false;
        bool playerDirectAngle = false;
        std::chrono::steady_clock::time_point nextTick{};
    };

    std::mutex g_spinLock;
    std::condition_variable g_spinWake;
    std::unordered_map<UInt32, std::shared_ptr<SpinState>> g_spins;
    bool g_spinSchedulerRunning = false;
    UInt64 g_spinGeneration = 0;
    std::once_flag g_spinnerConfigOnce;
    int g_maxActiveSpinners = kDefaultMaxActiveSpinners;

    std::string FormatFormId(UInt32 formId);
    bool ExecuteScriptLineQuiet(const std::string& command);
    int LoadMaxActiveSpinners();
    void EnsureSpinnerConfigLoaded();

    using CallFunctionNoWait_t = void (*)(VirtualMachine* vm, UInt64 unk1, VMIdentifier* vmIdentifier, const BSFixedString* fn, VMValue* args);
    RelocAddr<CallFunctionNoWait_t> g_CallFunctionNoWait(0x013D69D0);

    TESObjectREFR* LookupRef(UInt32 formId)
    {
        if (!formId) {
            return nullptr;
        }
        TESForm* form = LookupFormByID(formId);
        return form ? static_cast<TESObjectREFR*>(form) : nullptr;
    }

    float NormalizeRad(float v)
    {
        constexpr float twoPi = 6.28318530717958647692f;
        while (v > MATH_PI) {
            v -= twoPi;
        }
        while (v < -MATH_PI) {
            v += twoPi;
        }
        return v;
    }

    float NormalizeAngleDeg(float deg)
    {
        while (deg < 0.0f) {
            deg += 360.0f;
        }
        while (deg >= 360.0f) {
            deg -= 360.0f;
        }
        return deg;
    }

    bool IsActorRef(TESObjectREFR* ref)
    {
        return ref && ref->formType == kFormType_ACHR;
    }

    bool IsPlayerRef(TESObjectREFR* ref)
    {
        return ref && ref->formID == 0x00000014;
    }

    bool QueuePlayerDirectRotateZ(TESObjectREFR* ref, float angleRad)
    {
        if (!ref || !std::isfinite(angleRad) || !IsPlayerRef(ref)) {
            return false;
        }

        Actor* actor = static_cast<Actor*>(ref);
        actor->rot.z = NormalizeRad(angleRad);
        if (actor->middleProcess) {
            CALL_MEMBER_FN(actor->middleProcess, UpdateEquipment)(actor, 0x11);
        }
        return true;
    }

    bool QueueSetAngleZ(UInt32 formId, float angleRad)
    {
        if (!formId || !std::isfinite(angleRad)) {
            return false;
        }

        const float angleDeg = NormalizeAngleDeg(angleRad * kRadToDeg);
        const std::string command = FormatFormId(formId) + ".setangle z " + std::to_string(angleDeg);
        return ExecuteScriptLineQuiet(command);
    }

    bool QueueSettleObject(UInt32 formId)
    {
        if (!formId) {
            return false;
        }

        const std::string command = FormatFormId(formId) + ".modpos z 0";
        return ExecuteScriptLineQuiet(command);
    }

    bool CallObjectFunctionNoWait(TESObjectREFR* ref, const char* fnName, const std::vector<float>& floatArgs)
    {
        if (!ref || !fnName || !fnName[0] || !g_gameVM || !*g_gameVM || !(*g_gameVM)->m_virtualMachine) {
            return false;
        }

        VirtualMachine* vm = (*g_gameVM)->m_virtualMachine;

        VMValue receiver;
        PackValue(&receiver, &ref, vm);
        if (!receiver.IsIdentifier() || !receiver.data.id) {
            return false;
        }

        VMArray<VMVariable> args;
        for (float value : floatArgs) {
            VMVariable var;
            var.Set<float>(&value);
            args.Push(&var);
        }

        VMValue packedArgs;
        PackValue(&packedArgs, &args, vm);

        BSFixedString fn(fnName);
        g_CallFunctionNoWait(vm, 0, receiver.data.id, &fn, &packedArgs);
        return true;
    }

    bool QueueStopTranslation(TESObjectREFR* ref)
    {
        return CallObjectFunctionNoWait(ref, "StopTranslation", {});
    }

    bool QueueTranslateToZ(TESObjectREFR* ref, float angleRad, float speedDeg)
    {
        if (!ref || !std::isfinite(angleRad)) {
            return false;
        }

        if (speedDeg <= 0.0f || !std::isfinite(speedDeg)) {
            speedDeg = 45.0f;
        }

        const float x = ref->pos.x;
        const float y = ref->pos.y;
        const float z = ref->pos.z;
        const float ax = ref->rot.x * kRadToDeg;
        const float ay = ref->rot.y * kRadToDeg;
        const float az = NormalizeAngleDeg(angleRad * kRadToDeg);

        return CallObjectFunctionNoWait(ref, "TranslateTo", { x, y, z, ax, ay, az, 999999.0f, speedDeg });
    }

    bool IsCurrentSpinStateLocked(const std::shared_ptr<SpinState>& st)
    {
        if (!st) {
            return false;
        }
        const auto it = g_spins.find(st->key);
        return it != g_spins.end() && it->second.get() == st.get() && it->second->generation == st->generation;
    }

    bool IsCurrentSpinState(const std::shared_ptr<SpinState>& st)
    {
        std::lock_guard<std::mutex> guard(g_spinLock);
        return IsCurrentSpinStateLocked(st);
    }

    bool RemoveCurrentSpinState(const std::shared_ptr<SpinState>& st)
    {
        if (!st) {
            return false;
        }
        std::lock_guard<std::mutex> guard(g_spinLock);
        if (!IsCurrentSpinStateLocked(st)) {
            return false;
        }
        g_spins.erase(st->key);
        st->active.store(false);
        g_spinWake.notify_all();
        return true;
    }

    class SpinTask final : public ITaskDelegate
    {
    public:
        SpinTask(std::shared_ptr<SpinState> state, bool restore)
            : st_(std::move(state)), restore_(restore)
        {
        }

        void Run() override
        {
            if (!st_) {
                return;
            }

            TESObjectREFR* target = LookupRef(st_->targetId);
            if (!target) {
                RemoveCurrentSpinState(st_);
                return;
            }

            if (restore_) {
                bool hasBase = false;
                float baseAngle = 0.0f;
                {
                    std::lock_guard<std::mutex> guard(st_->lock);
                    hasBase = st_->hasBaseAngle;
                    baseAngle = st_->baseAngleRad;
                }
                QueueStopTranslation(target);
                if (hasBase) {
                    if (st_->playerDirectAngle) {
                        QueuePlayerDirectRotateZ(target, baseAngle);
                    }
                    else {
                        QueueSetAngleZ(st_->targetId, baseAngle);
                    }
                }
                if (!IsActorRef(target)) {
                    QueueSettleObject(st_->targetId);
                }
                return;
            }

            if (!st_->active.load() || !IsCurrentSpinState(st_)) {
                return;
            }

            float nextAngle = 0.0f;
            bool playerMissing = false;
            bool hasBaseForRestore = false;
            float baseAngleForRestore = 0.0f;
            {
                std::lock_guard<std::mutex> guard(st_->lock);
                if (!st_->hasBaseAngle) {
                    st_->baseAngleRad = target->rot.z;
                    st_->phaseRad = 0.0f;
                    st_->hasBaseAngle = true;
                }

                if (st_->mode == 2 && st_->playerId) {
                    TESObjectREFR* player = LookupRef(st_->playerId);
                    if (!player) {
                        playerMissing = true;
                        nextAngle = st_->baseAngleRad;
                    }
                    else {
                        const float dx = player->pos.x - target->pos.x;
                        const float dy = player->pos.y - target->pos.y;
                        if (std::fabs(dx) > 0.001f || std::fabs(dy) > 0.001f) {
                            nextAngle = std::atan2(dx, dy);
                        }
                        else {
                            nextAngle = st_->baseAngleRad;
                        }
                    }
                }
                else {
                    const int tickMs = st_->playerDirectAngle ? kPlayerDirectSpinTickMs : kObjectSpinTickMs;
                    const float stepDeg = (st_->stepAngle || st_->playerDirectAngle)
                        ? std::max(0.05f, st_->speedDeg * (static_cast<float>(tickMs) / 1000.0f))
                        : kSpinSegmentDeg;
                    st_->phaseRad = NormalizeRad(st_->phaseRad + (stepDeg * st_->dir * kDegToRad));
                    nextAngle = NormalizeRad(st_->baseAngleRad + st_->phaseRad);
                }

                hasBaseForRestore = st_->hasBaseAngle;
                baseAngleForRestore = st_->baseAngleRad;
            }

            if (playerMissing) {
                RemoveCurrentSpinState(st_);
                QueueStopTranslation(target);
                if (hasBaseForRestore) {
                    if (st_->playerDirectAngle) {
                        QueuePlayerDirectRotateZ(target, baseAngleForRestore);
                    }
                    else {
                        QueueSetAngleZ(st_->targetId, baseAngleForRestore);
                    }
                }
                if (!IsActorRef(target)) {
                    QueueSettleObject(st_->targetId);
                }
                return;
            }

            if (!IsCurrentSpinState(st_)) {
                return;
            }

            if (st_->playerDirectAngle) {
                QueuePlayerDirectRotateZ(target, nextAngle);
            }
            else if (st_->mode == 2 || st_->stepAngle) {
                QueueSetAngleZ(st_->targetId, nextAngle);
            }
            else {
                QueueTranslateToZ(target, nextAngle, st_->speedDeg);
            }
        }

    private:
        std::shared_ptr<SpinState> st_;
        bool restore_ = false;
    };

    class PauseTask final : public ITaskDelegate
    {
    public:
        explicit PauseTask(std::shared_ptr<SpinState> state)
            : st_(std::move(state))
        {
        }

        void Run() override
        {
            if (!st_) {
                return;
            }

            TESObjectREFR* target = LookupRef(st_->targetId);
            if (!target) {
                return;
            }

            QueueStopTranslation(target);
            if (!IsActorRef(target)) {
                QueueSettleObject(st_->targetId);
            }
        }

    private:
        std::shared_ptr<SpinState> st_;
    };

    void QueueSpinTask(const std::shared_ptr<SpinState>& st, bool restore)
    {
        if (!g_task || !st) {
            return;
        }
        g_task->AddTask(new SpinTask(st, restore));
    }

    void QueueRestoreTask(const std::shared_ptr<SpinState>& st)
    {
        if (!st || st->restoreQueued.exchange(true)) {
            return;
        }
        QueueSpinTask(st, true);
    }

    void QueuePauseTask(const std::shared_ptr<SpinState>& st)
    {
        if (!g_task || !st) {
            return;
        }
        g_task->AddTask(new PauseTask(st));
    }

    std::chrono::milliseconds GetSpinInterval(const SpinState& st)
    {
        if (st.playerDirectAngle) {
            return std::chrono::milliseconds(kPlayerDirectSpinTickMs);
        }

        if (st.mode == 2 || st.stepAngle) {
            return std::chrono::milliseconds(st.mode == 2 ? kFaceTickMs : kObjectSpinTickMs);
        }

        float speedDeg = st.speedDeg;
        if (speedDeg <= 0.0f || !std::isfinite(speedDeg)) {
            speedDeg = 45.0f;
        }

        float segSeconds = kSpinSegmentDeg / speedDeg;
        if (segSeconds < 0.20f || !std::isfinite(segSeconds)) {
            segSeconds = 0.20f;
        }
        return std::chrono::milliseconds(static_cast<int>(segSeconds * 1000.0f));
    }

    void SpinSchedulerLoop()
    {
        for (;;) {
            std::vector<std::shared_ptr<SpinState>> due;
            {
                std::unique_lock<std::mutex> guard(g_spinLock);
                if (g_spins.empty()) {
                    g_spinSchedulerRunning = false;
                    return;
                }

                const auto now = std::chrono::steady_clock::now();
                auto nextWake = now + std::chrono::seconds(1);

                for (auto& entry : g_spins) {
                    auto& st = entry.second;
                    if (!st || !st->active.load() || st->paused.load()) {
                        continue;
                    }
                    if (st->nextTick <= now) {
                        due.push_back(st);
                        st->nextTick = now + GetSpinInterval(*st);
                    }
                    if (st->nextTick < nextWake) {
                        nextWake = st->nextTick;
                    }
                }

                if (due.empty()) {
                    g_spinWake.wait_until(guard, nextWake);
                    continue;
                }
            }

            for (auto& st : due) {
                QueueSpinTask(st, false);
            }
        }
    }

    void EnsureSpinSchedulerRunningLocked()
    {
        if (g_spinSchedulerRunning) {
            return;
        }
        g_spinSchedulerRunning = true;
        std::thread(SpinSchedulerLoop).detach();
    }

    std::vector<std::shared_ptr<SpinState>> StopAllNativeSpinners(bool queueRestore)
    {
        std::vector<std::shared_ptr<SpinState>> states;
        {
            std::lock_guard<std::mutex> guard(g_spinLock);
            for (auto& entry : g_spins) {
                states.push_back(entry.second);
            }
            g_spins.clear();
            g_spinWake.notify_all();
        }

        for (auto& st : states) {
            if (st) {
                st->active.store(false);
                if (queueRestore) {
                    QueueRestoreTask(st);
                }
                else {
                    QueuePauseTask(st);
                }
            }
        }

        return states;
    }

    std::vector<std::shared_ptr<SpinState>> PauseAllNativeSpinners()
    {
        std::vector<std::shared_ptr<SpinState>> states;
        {
            std::lock_guard<std::mutex> guard(g_spinLock);
            for (auto& entry : g_spins) {
                if (entry.second) {
                    states.push_back(entry.second);
                    entry.second->paused.store(true);
                }
            }
            g_spinWake.notify_all();
        }

        for (auto& st : states) {
            QueuePauseTask(st);
        }

        return states;
    }

    int GetActiveSpinnerCount()
    {
        std::lock_guard<std::mutex> guard(g_spinLock);
        return static_cast<int>(g_spins.size());
    }

    bool IsNativeSpinnerActive(TESObjectREFR* target)
    {
        if (!target) {
            return false;
        }
        std::lock_guard<std::mutex> guard(g_spinLock);
        return g_spins.find(target->formID) != g_spins.end();
    }

    bool StartNativeSpinner(TESObjectREFR* target, TESObjectREFR* player, int mode, float speedDeg, float dir)
    {
        if (!g_task || !target) {
            return false;
        }

        EnsureSpinnerConfigLoaded();

        if (mode == 2 && IsPlayerRef(target)) {
            return false;
        }

        if (speedDeg <= 0.0f || !std::isfinite(speedDeg)) {
            speedDeg = 45.0f;
        }
        if (std::fabs(dir) < 0.001f || !std::isfinite(dir)) {
            dir = -1.0f;
        }

        std::shared_ptr<SpinState> oldState;
        bool haveOldBase = false;
        float oldBase = 0.0f;
        bool shouldDelay = false;

        auto st = std::make_shared<SpinState>();
        st->targetId = target->formID;
        st->playerId = player ? player->formID : 0;
        st->key = target->formID;
        st->mode = mode;
        st->speedDeg = speedDeg;
        st->dir = dir < 0.0f ? -1.0f : 1.0f;
        st->stepAngle = (mode == 1 && !IsActorRef(target));
        st->playerDirectAngle = (mode == 1 && IsPlayerRef(target));
        st->active.store(true);
        st->paused.store(false);

        {
            std::lock_guard<std::mutex> guard(g_spinLock);
            const auto existing = g_spins.find(st->key);
            if (existing == g_spins.end() && static_cast<int>(g_spins.size()) >= g_maxActiveSpinners) {
                return false;
            }

            if (existing != g_spins.end()) {
                oldState = existing->second;
                g_spins.erase(existing);
                if (oldState) {
                    oldState->active.store(false);
                    oldState->paused.store(false);
                    std::lock_guard<std::mutex> oldGuard(oldState->lock);
                    if (oldState->hasBaseAngle) {
                        haveOldBase = true;
                        oldBase = oldState->baseAngleRad;
                    }
                    shouldDelay = true;
                }
            }

            st->generation = ++g_spinGeneration;
            if (haveOldBase) {
                st->baseAngleRad = oldBase;
                st->phaseRad = NormalizeRad(target->rot.z - oldBase);
                st->hasBaseAngle = true;
            }
            st->nextTick = std::chrono::steady_clock::now() + std::chrono::milliseconds(shouldDelay ? kSwitchDelayMs : 0);
            g_spins[st->key] = st;
            EnsureSpinSchedulerRunningLocked();
            g_spinWake.notify_all();
        }

        if (oldState) {
            QueuePauseTask(oldState);
        }

        return true;
    }




    struct alignas(16) TESScript
    {
        UInt8 bytes[0x178];
    };

    static_assert(sizeof(TESScript) == 0x180, "Unexpected aligned TESScript storage size");

    using TESScript_Constructor_t = TESScript* (__fastcall*)(TESScript* script);
    using TESScript_Execute_t = bool(__fastcall*)(TESScript* script, UInt64 unk01, UInt64 unk02, UInt64 unk03, bool unk04);
    using TESScript_MarkAsTemporary_t = void(__fastcall*)(TESScript* script);
    using TESScript_Compile_t = bool(__fastcall*)(TESScript* script, void* globalScript, int compilerTypeIndex, UInt64 unk03);
    using TESScript_CompileAndRun_t = bool(__fastcall*)(TESScript* script, void* globalScript, int compilerTypeIndex, void* unk03);
    using TESScript_SetText_t = void(__fastcall*)(TESScript* script, char* scriptText);
    using TESScript_Destructor_t = void(__fastcall*)(TESScript* script);

    TESScript_Constructor_t g_TESScript_Constructor = nullptr;
    TESScript_Execute_t g_TESScript_Execute = nullptr;
    TESScript_MarkAsTemporary_t g_TESScript_MarkAsTemporary = nullptr;
    TESScript_Compile_t g_TESScript_Compile = nullptr;
    TESScript_CompileAndRun_t g_TESScript_CompileAndRun = nullptr;
    TESScript_SetText_t g_TESScript_SetText = nullptr;
    TESScript_Destructor_t g_TESScript_Destructor = nullptr;
    UInt64 g_GlobalScriptStateAddress = 0;
    bool g_TESScriptInitAttempted = false;
    bool g_TESScriptReady = false;

    std::filesystem::path GetGameRootDirectory()
    {
        char exePath[MAX_PATH]{};
        const DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(exePath).parent_path();
    }


    std::filesystem::path GetPluginDirectory()
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&GetPluginDirectory), &module)) {
            char dllPath[MAX_PATH]{};
            const DWORD len = GetModuleFileNameA(module, dllPath, MAX_PATH);
            if (len != 0 && len < MAX_PATH) {
                return std::filesystem::path(dllPath).parent_path();
            }
        }

        return GetGameRootDirectory() / "Data" / "F4SE" / "Plugins";
    }

    std::filesystem::path GetRuntimePluginDirectory()
    {



        std::error_code ec;
        const auto dir = GetGameRootDirectory() / "Data" / "F4SE" / "Plugins";
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::filesystem::path GetLogPath()
    {
        const char* userProfile = std::getenv("USERPROFILE");
        if (userProfile && userProfile[0]) {
            auto p = std::filesystem::path(userProfile) / "Documents" / "My Games" / "Fallout4" / "F4SE";
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            return p / "Straw_SamExtensions.log";
        }
        return GetGameRootDirectory() / "Data" / "F4SE" / "Plugins" / "Straw_SamExtensions.log";
    }

    void Log(const char* fmt, ...)
    {
        if constexpr (!kEnableFileLog) {
            return;
        }

        char buffer[4096]{};
        va_list args;
        va_start(args, fmt);
        vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
        va_end(args);

        try {
            std::ofstream out(GetLogPath(), std::ios::app);
            out << buffer << '\n';
        }
        catch (...) {

        }
    }

    void EchoCommandToConsole(const std::string& command)
    {
        if (command.empty()) {
            return;
        }



        Console_Print("> %s", command.c_str());
    }

    std::string ToLowerAscii(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    std::string TrimAscii(const std::string& input)
    {
        const auto first = std::find_if_not(input.begin(), input.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });
        if (first == input.end()) {
            return {};
        }
        const auto last = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }).base();
        return std::string(first, last);
    }

    std::string StripMatchingQuotes(const std::string& input)
    {
        std::string s = TrimAscii(input);
        if (s.size() >= 2) {
            const char first = s.front();
            const char last = s.back();
            if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
                s = s.substr(1, s.size() - 2);
            }
        }
        return TrimAscii(s);
    }


    std::string StripIniInlineComment(const std::string& input)
    {
        bool inQuote = false;
        char quoteChar = '\0';
        for (std::size_t i = 0; i < input.size(); ++i) {
            const char c = input[i];
            if ((c == '"' || c == '\'') && (i == 0 || input[i - 1] != '\\')) {
                if (!inQuote) {
                    inQuote = true;
                    quoteChar = c;
                }
                else if (quoteChar == c) {
                    inQuote = false;
                    quoteChar = '\0';
                }
                continue;
            }
            if (!inQuote) {
                if (c == ';' || c == '#') {
                    return TrimAscii(input.substr(0, i));
                }
                if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
                    return TrimAscii(input.substr(0, i));
                }
            }
        }
        return TrimAscii(input);
    }

    std::string NormalizeTxtFilterName(const std::string& input)
    {
        std::string value = StripMatchingQuotes(input);
        if (value.empty()) {
            return {};
        }

        std::replace(value.begin(), value.end(), '\\', '/');
        const auto slash = value.find_last_of('/');
        if (slash != std::string::npos) {
            value = value.substr(slash + 1);
        }

        value = ToLowerAscii(TrimAscii(value));
        if (value.size() > 4 && value.substr(value.size() - 4) == ".txt") {
            value.resize(value.size() - 4);
        }

        return value;
    }

    void AddExcludeToken(std::unordered_set<std::string>& excludes, const std::string& token)
    {
        const auto normalized = NormalizeTxtFilterName(token);
        if (!normalized.empty()) {
            excludes.insert(normalized);
        }
    }

    void AddExcludeList(std::unordered_set<std::string>& excludes, const std::string& value)
    {
        std::string current;
        for (const char c : value) {
            if (c == ',' || c == '|') {
                AddExcludeToken(excludes, current);
                current.clear();
            }
            else {
                current.push_back(c);
            }
        }
        AddExcludeToken(excludes, current);
    }

    bool ParseIniBool(const std::string& value, bool defaultValue)
    {
        const auto v = ToLowerAscii(StripMatchingQuotes(value));
        if (v == "1" || v == "true" || v == "yes" || v == "on") {
            return true;
        }
        if (v == "0" || v == "false" || v == "no" || v == "off") {
            return false;
        }
        return defaultValue;
    }

    int ParseIniInt(const std::string& value, int defaultValue)
    {
        const auto v = StripMatchingQuotes(value);
        if (v.empty()) {
            return defaultValue;
        }
        char* end = nullptr;
        const long parsed = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str()) {
            return defaultValue;
        }
        return static_cast<int>(parsed);
    }

    std::vector<std::filesystem::path> GetIniCandidatePaths()
    {
        std::vector<std::filesystem::path> paths;
        paths.push_back(GetPluginDirectory() / "Straw_SamExtensions.ini");
        paths.push_back(GetGameRootDirectory() / "Data" / "F4SE" / "Plugins" / "Straw_SamExtensions.ini");
        paths.push_back(std::filesystem::current_path() / "Data" / "F4SE" / "Plugins" / "Straw_SamExtensions.ini");
        return paths;
    }

    int LoadMaxActiveSpinners()
    {
        int maxActive = kDefaultMaxActiveSpinners;
        std::ifstream ini;
        for (const auto& iniPath : GetIniCandidatePaths()) {
            ini.open(iniPath);
            if (ini) {
                break;
            }
            ini.clear();
        }

        if (ini) {
            std::string line;
            while (std::getline(ini, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                line = TrimAscii(line);
                if (line.empty()) {
                    continue;
                }
                if (line[0] == ';' || line[0] == '#') {
                    continue;
                }
                if (line.size() >= 2 && line[0] == '/' && line[1] == '/') {
                    continue;
                }
                if (line.front() == '[' && line.back() == ']') {
                    continue;
                }

                const auto equals = line.find('=');
                if (equals == std::string::npos) {
                    continue;
                }

                const auto key = ToLowerAscii(TrimAscii(line.substr(0, equals)));
                const auto value = StripIniInlineComment(line.substr(equals + 1));
                if (key == "maxactivespinners") {
                    maxActive = ParseIniInt(value, maxActive);
                }
            }
        }

        return std::max(1, std::min(kHardMaxActiveSpinners, maxActive));
    }

    void EnsureSpinnerConfigLoaded()
    {
        std::call_once(g_spinnerConfigOnce, []() {
            g_maxActiveSpinners = LoadMaxActiveSpinners();
        });
    }

    std::unordered_set<std::string> LoadRootTxtExcludes()
    {
        bool useDefaultExcludes = true;
        std::unordered_set<std::string> userExcludes;

        std::ifstream ini;
        for (const auto& iniPath : GetIniCandidatePaths()) {
            ini.open(iniPath);
            if (ini) {
                break;
            }
            ini.clear();
        }

        if (ini) {
            std::string line;
            while (std::getline(ini, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                line = TrimAscii(line);
                if (line.empty()) {
                    continue;
                }
                if (line[0] == ';' || line[0] == '#') {
                    continue;
                }
                if (line.size() >= 2 && line[0] == '/' && line[1] == '/') {
                    continue;
                }
                if (line.front() == '[' && line.back() == ']') {
                    continue;
                }

                const auto equals = line.find('=');
                if (equals == std::string::npos) {
                    AddExcludeToken(userExcludes, line);
                    continue;
                }

                const auto key = ToLowerAscii(TrimAscii(line.substr(0, equals)));
                const auto value = StripIniInlineComment(line.substr(equals + 1));

                if (key == "usedefaultexcludes" || key == "usebuiltindefaultexcludes") {
                    useDefaultExcludes = ParseIniBool(value, useDefaultExcludes);
                    continue;
                }

                if (key == "exclude" || key == "excludefile" || key == "excludetxtfile" ||
                    key == "excludedtxtfile" || key == "excludedtxtfiles" || key == "hide" || key == "hidefile") {
                    AddExcludeList(userExcludes, value);
                }
            }
        }

        std::unordered_set<std::string> excludes;
        if (useDefaultExcludes) {
            static const char* const builtInExcludes[] = {
                "CustomControlMap.txt",
                "EditorTips.txt",
                "EditorWarnings.txt",
                "f4se_readme.txt",
                "f4se_whatsnew.txt",
                "nvdebris.txt",
                "sam.txt",
                "TemporaryBehaviorEventInfoOutput.txt",
                "TemporaryClipDataOutput.txt",
                "TemporarySyncAnimDataOutput.txt"
            };

            for (const char* fileName : builtInExcludes) {
                AddExcludeToken(excludes, fileName);
            }
        }

        excludes.insert(userExcludes.begin(), userExcludes.end());
        return excludes;
    }

    bool IsRootTxtExcluded(const std::filesystem::path& path, const std::unordered_set<std::string>& excludes)
    {
        const auto normalized = NormalizeTxtFilterName(path.filename().string());
        return !normalized.empty() && excludes.find(normalized) != excludes.end();
    }

    bool IsSafeBatchStem(const std::string& name)
    {
        if (name.empty() || name.size() > 240) {
            return false;
        }
        if (name == "." || name == ".." || name.find("..") != std::string::npos) {
            return false;
        }

        for (const unsigned char c : name) {
            if (c < 32) {
                return false;
            }
            switch (c) {
            case '\\': case '/': case ':': case '*': case '?': case '"': case '<': case '>': case '|':
                return false;
            default:
                break;
            }
        }
        return true;
    }

    std::vector<std::string> ScanRootTxtFileStems()
    {
        std::vector<std::string> names;
        const auto root = GetGameRootDirectory();
        const auto excludes = LoadRootTxtExcludes();

        try {
            for (const auto& entry : std::filesystem::directory_iterator(root)) {
                if (!entry.is_regular_file()) {
                    continue;
                }

                const auto path = entry.path();
                if (ToLowerAscii(path.extension().string()) != ".txt") {
                    continue;
                }

                if (IsRootTxtExcluded(path, excludes)) {
                    continue;
                }

                const auto stem = path.stem().string();
                if (IsSafeBatchStem(stem)) {
                    names.push_back(stem);
                }
            }
        }
        catch (const std::exception& e) {
            Log("Straw_SamExtensions: root TXT scan failed: %s", e.what());
        }
        catch (...) {
            Log("Straw_SamExtensions: root TXT scan failed with an unknown exception");
        }

        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
            return ToLowerAscii(a) < ToLowerAscii(b);
        });
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    bool IsKnownRootTxtStem(const std::string& name)
    {
        const auto wanted = ToLowerAscii(name);
        const auto names = ScanRootTxtFileStems();
        return std::any_of(names.begin(), names.end(), [&](const std::string& existing) {
            return ToLowerAscii(existing) == wanted;
        });
    }

    std::string FormatFormId(UInt32 formId)
    {
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "%08X", formId);
        return std::string(buffer);
    }

    std::string GetFirstTokenLower(const std::string& command)
    {
        const auto trimmed = TrimAscii(command);
        if (trimmed.empty()) {
            return {};
        }

        const auto end = std::find_if(trimmed.begin(), trimmed.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });

        return ToLowerAscii(std::string(trimmed.begin(), end));
    }

    bool HasExplicitRefPrefix(const std::string& command)
    {
        const auto trimmed = TrimAscii(command);
        if (trimmed.empty()) {
            return false;
        }

        std::size_t i = 0;
        while (i < trimmed.size() && std::isxdigit(static_cast<unsigned char>(trimmed[i]))) {
            ++i;
        }

        return i >= 4 && i <= 8 && i < trimmed.size() && trimmed[i] == '.';
    }

    bool ShouldPrefixSelectedRef(const std::string& command)
    {
        if (HasExplicitRefPrefix(command)) {
            return false;
        }

        const auto token = GetFirstTokenLower(command);
        if (token.empty()) {
            return false;
        }



        static const std::unordered_set<std::string> targetCommands = {
            "additem", "addkeyword", "disable", "disableai", "drop",
            "enable", "equipitem", "getangle", "getav", "getavinfo",
            "getpos", "inv", "kill", "markfordelete", "mfg",
            "modangle", "modav", "moveto", "openactorcontainer", "playidle",
            "recycleactor", "removeallitems", "removeitem", "removekeyword",
            "resetai", "resurrect", "setactoralpha", "setangle", "setav",
            "setessential", "setghost", "setlevel", "setnpcweight", "setpos",
            "setscale", "tai", "tc", "unequipall", "unequipitem"
        };

        return targetCommands.find(token) != targetCommands.end();
    }

    std::string ApplySelectedRefPrefixIfNeeded(const std::string& command, UInt32 targetFormId)
    {
        if (targetFormId == 0 || !ShouldPrefixSelectedRef(command)) {
            return command;
        }

        return FormatFormId(targetFormId) + "." + command;
    }

    UInt64 FindSignature(const MODULEINFO& moduleInfo, const unsigned char* pattern, const char* mask, int offset)
    {
        const auto start = reinterpret_cast<UInt64>(moduleInfo.lpBaseOfDll);
        const auto size = static_cast<UInt64>(moduleInfo.SizeOfImage);
        const auto patternSize = static_cast<UInt64>(std::strlen(mask));

        if (start == 0 || size <= patternSize) {
            return 0;
        }

        const auto end = start + size - patternSize;
        for (UInt64 i = start; i < end; ++i) {
            bool matched = true;
            for (UInt64 j = 0; j < patternSize; ++j) {
                if (mask[j] == '?') {
                    continue;
                }
                if (pattern[j] != *reinterpret_cast<const unsigned char*>(i + j)) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                return static_cast<UInt64>(static_cast<SInt64>(i) + offset);
            }
        }

        return 0;
    }

    UInt64 ParseRipRelativeAddress(UInt64 address, UInt8 skipCount)
    {
        const SInt32 offset = *reinterpret_cast<SInt32*>(address + skipCount);
        const UInt64 rip = address + skipCount + sizeof(offset);
        return rip + offset;
    }

    bool InitTESScriptRuntime()
    {
        if (g_TESScriptInitAttempted) {
            return g_TESScriptReady;
        }
        g_TESScriptInitAttempted = true;

        MODULEINFO moduleInfo{};
        HMODULE mainModule = GetModuleHandleA(nullptr);
        if (!mainModule) {
            Log("Straw_SamExtensions: InitTESScriptRuntime failed: no main module");
            return false;
        }

        if (!GetModuleInformation(GetCurrentProcess(), mainModule, &moduleInfo, sizeof(moduleInfo))) {
            Log("Straw_SamExtensions: InitTESScriptRuntime failed: GetModuleInformation failed (%lu)", GetLastError());
            return false;
        }

        const unsigned char scriptStatePattern[] = { 0x75, 0xF7, 0x85, 0xC0, 0x74, 0x32 };
        const UInt64 scriptStateMarker = FindSignature(moduleInfo, scriptStatePattern, "xxxxxx", 0x6);
        if (!scriptStateMarker) {
            Log("Straw_SamExtensions: InitTESScriptRuntime failed: script state marker not found");
            return false;
        }

        const unsigned char scriptPattern[] = { 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0x78 };
        const UInt64 scriptMarker = FindSignature(moduleInfo, scriptPattern, "xxxxxxxxxxx", -0x4E);
        if (!scriptMarker) {
            Log("Straw_SamExtensions: InitTESScriptRuntime failed: TESScript marker not found");
            return false;
        }

        const UInt64 compileAndRun = ParseRipRelativeAddress(scriptMarker + 0x59, 1);
        g_GlobalScriptStateAddress = ParseRipRelativeAddress(scriptStateMarker, 3);
        g_TESScript_Constructor = reinterpret_cast<TESScript_Constructor_t>(ParseRipRelativeAddress(scriptMarker + 0x18, 1));
        g_TESScript_MarkAsTemporary = reinterpret_cast<TESScript_MarkAsTemporary_t>(ParseRipRelativeAddress(scriptMarker + 0x22, 1));
        g_TESScript_SetText = reinterpret_cast<TESScript_SetText_t>(ParseRipRelativeAddress(scriptMarker + 0x2F, 1));
        g_TESScript_Compile = reinterpret_cast<TESScript_Compile_t>(ParseRipRelativeAddress(compileAndRun + 0x47, 1));
        g_TESScript_Execute = reinterpret_cast<TESScript_Execute_t>(ParseRipRelativeAddress(compileAndRun + 0x75, 1));
        g_TESScript_CompileAndRun = reinterpret_cast<TESScript_CompileAndRun_t>(compileAndRun);
        g_TESScript_Destructor = reinterpret_cast<TESScript_Destructor_t>(ParseRipRelativeAddress(scriptMarker + 0x63, 1));

        g_TESScriptReady =
            g_GlobalScriptStateAddress &&
            g_TESScript_Constructor &&
            g_TESScript_MarkAsTemporary &&
            g_TESScript_SetText &&
            g_TESScript_Compile &&
            g_TESScript_Execute &&
            g_TESScript_CompileAndRun &&
            g_TESScript_Destructor;

        Log("Straw_SamExtensions: TESScript init %s. state=%p ctor=%p setText=%p compileRun=%p dtor=%p",
            g_TESScriptReady ? "OK" : "FAILED",
            reinterpret_cast<void*>(g_GlobalScriptStateAddress),
            reinterpret_cast<void*>(g_TESScript_Constructor),
            reinterpret_cast<void*>(g_TESScript_SetText),
            reinterpret_cast<void*>(g_TESScript_CompileAndRun),
            reinterpret_cast<void*>(g_TESScript_Destructor));

        return g_TESScriptReady;
    }

    bool ExecuteScriptLineInternal(const std::string& command, bool echo)
    {
        if (!InitTESScriptRuntime()) {
            return false;
        }

        if (command.empty() || command.size() > kMaxCommandLength) {
            Log("Straw_SamExtensions: rejected command due to length/empty");
            return false;
        }

        if (echo) {
            EchoCommandToConsole(command);
        }

        std::vector<char> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back('\0');

        TESScript scriptObject{};
        g_TESScript_Constructor(&scriptObject);
        g_TESScript_MarkAsTemporary(&scriptObject);
        g_TESScript_SetText(&scriptObject, mutableCommand.data());

        void* globalScript = nullptr;
        if (g_GlobalScriptStateAddress) {
            globalScript = *reinterpret_cast<void**>(g_GlobalScriptStateAddress);
        }

        const bool ok = g_TESScript_CompileAndRun(&scriptObject, globalScript, kSysWindowCompileAndRun, nullptr);
        g_TESScript_Destructor(&scriptObject);

        if (echo) {
            Log("Straw_SamExtensions: command %s: %s", ok ? "OK" : "FAILED", command.c_str());
        }
        return ok;
    }

    bool ExecuteScriptLine(const std::string& command)
    {
        return ExecuteScriptLineInternal(command, true);
    }

    bool ExecuteScriptLineQuiet(const std::string& command)
    {
        return ExecuteScriptLineInternal(command, false);
    }

    bool ShouldSkipBatchLine(const std::string& line)
    {
        if (line.empty()) {
            return true;
        }
        if (line[0] == ';' || line[0] == '#') {
            return true;
        }
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/') {
            return true;
        }
        return false;
    }

    constexpr float kRadiansToDegrees = 57.29577951308232f;


    void MatrixToEulerXYZDegreesRaw(const float matrix[3][3], float& yaw, float& pitch, float& roll)
    {



        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        if (matrix[0][2] < 1.0f) {
            if (matrix[0][2] > -1.0f) {
                x = std::atan2(-matrix[1][2], matrix[2][2]);
                y = std::asin(matrix[0][2]);
                z = std::atan2(-matrix[0][1], matrix[0][0]);
            }
            else {
                x = -std::atan2(-matrix[1][0], matrix[1][1]);
                y = static_cast<float>(-MATH_PI / 2.0);
                z = 0.0f;
            }
        }
        else {
            x = std::atan2(matrix[1][0], matrix[1][1]);
            y = static_cast<float>(MATH_PI / 2.0);
            z = 0.0f;
        }

        yaw = x * kRadiansToDegrees;
        pitch = y * kRadiansToDegrees;
        roll = z * kRadiansToDegrees;
    }

    void MatrixFromEulerXYZDegreesRaw(float yaw, float pitch, float roll, float out[3][3])
    {


        const float x = yaw / kRadiansToDegrees;
        const float y = pitch / kRadiansToDegrees;
        const float z = roll / kRadiansToDegrees;

        const float cx = std::cos(x);
        const float sx = std::sin(x);
        const float cy = std::cos(y);
        const float sy = std::sin(y);
        const float cz = std::cos(z);
        const float sz = std::sin(z);

        out[0][0] = cy * cz;
        out[0][1] = -cy * sz;
        out[0][2] = sy;

        out[1][0] = sx * sy * cz + cx * sz;
        out[1][1] = -sx * sy * sz + cx * cz;
        out[1][2] = -sx * cy;

        out[2][0] = -cx * sy * cz + sx * sz;
        out[2][1] = cx * sy * sz + sx * cz;
        out[2][2] = cx * cy;
    }


    void MatrixMirrorLocalZ(const float source[3][3], float out[3][3])
    {


        out[0][0] = source[0][0];
        out[0][1] = source[0][1];
        out[0][2] = -source[0][2];
        out[1][0] = source[1][0];
        out[1][1] = source[1][1];
        out[1][2] = -source[1][2];
        out[2][0] = -source[2][0];
        out[2][1] = -source[2][1];
        out[2][2] = source[2][2];
    }

    void MatrixMirrorWorldZLocalX(const float source[3][3], float out[3][3])
    {
        out[0][0] = -source[0][0];
        out[0][1] = source[0][1];
        out[0][2] = source[0][2];
        out[1][0] = -source[1][0];
        out[1][1] = source[1][1];
        out[1][2] = source[1][2];
        out[2][0] = source[2][0];
        out[2][1] = -source[2][1];
        out[2][2] = -source[2][2];
    }

    void MatrixMultiply3x3(const float left[3][3], const float right[3][3], float out[3][3])
    {
        float temp[3][3]{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                temp[r][c] = left[r][0] * right[0][c] + left[r][1] * right[1][c] + left[r][2] * right[2][c];
            }
        }
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out[r][c] = temp[r][c];
            }
        }
    }

    void MatrixAxisAngleRadians(float axisX, float axisY, float axisZ, float angle, float out[3][3])
    {
        const float lenSq = axisX * axisX + axisY * axisY + axisZ * axisZ;
        if (lenSq <= 0.000001f) {
            out[0][0] = 1.0f; out[0][1] = 0.0f; out[0][2] = 0.0f;
            out[1][0] = 0.0f; out[1][1] = 1.0f; out[1][2] = 0.0f;
            out[2][0] = 0.0f; out[2][1] = 0.0f; out[2][2] = 1.0f;
            return;
        }

        const float invLen = 1.0f / std::sqrt(lenSq);
        const float x = axisX * invLen;
        const float y = axisY * invLen;
        const float z = axisZ * invLen;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float t = 1.0f - c;

        out[0][0] = t * x * x + c;
        out[0][1] = t * x * y - s * z;
        out[0][2] = t * x * z + s * y;
        out[1][0] = t * x * y + s * z;
        out[1][1] = t * y * y + c;
        out[1][2] = t * y * z - s * x;
        out[2][0] = t * x * z - s * y;
        out[2][1] = t * y * z + s * x;
        out[2][2] = t * z * z + c;
    }

    float NormalizeDegrees180(float angle)
    {
        while (angle > 180.0f) {
            angle -= 360.0f;
        }
        while (angle < -180.0f) {
            angle += 360.0f;
        }
        return angle;
    }

    void MatrixToQuaternion(const float m[3][3], float& w, float& x, float& y, float& z)
    {
        const float trace = m[0][0] + m[1][1] + m[2][2];
        if (trace > 0.0f) {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            w = 0.25f * s;
            x = (m[2][1] - m[1][2]) / s;
            y = (m[0][2] - m[2][0]) / s;
            z = (m[1][0] - m[0][1]) / s;
        }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
            const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
            w = (m[2][1] - m[1][2]) / s;
            x = 0.25f * s;
            y = (m[0][1] + m[1][0]) / s;
            z = (m[0][2] + m[2][0]) / s;
        }
        else if (m[1][1] > m[2][2]) {
            const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
            w = (m[0][2] - m[2][0]) / s;
            x = (m[0][1] + m[1][0]) / s;
            y = 0.25f * s;
            z = (m[1][2] + m[2][1]) / s;
        }
        else {
            const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
            w = (m[1][0] - m[0][1]) / s;
            x = (m[0][2] + m[2][0]) / s;
            y = (m[1][2] + m[2][1]) / s;
            z = 0.25f * s;
        }

        const float lenSq = w * w + x * x + y * y + z * z;
        if (lenSq > 0.000001f) {
            const float invLen = 1.0f / std::sqrt(lenSq);
            w *= invLen;
            x *= invLen;
            y *= invLen;
            z *= invLen;
        }
        else {
            w = 1.0f;
            x = 0.0f;
            y = 0.0f;
            z = 0.0f;
        }
    }

    float MatrixLocalZTwistDegrees(const float m[3][3])
    {
        float w = 1.0f;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        MatrixToQuaternion(m, w, x, y, z);

        const float lenSq = w * w + z * z;
        if (lenSq <= 0.000001f) {
            return 0.0f;
        }

        const float invLen = 1.0f / std::sqrt(lenSq);
        w *= invLen;
        z *= invLen;

        return NormalizeDegrees180(2.0f * std::atan2(z, w) * kRadiansToDegrees);
    }

    float MatrixLocalZTwistDeltaDegrees(const float a, const float b)
    {
        return NormalizeDegrees180(a - b);
    }

    void CopyMatrix3x3(const float source[3][3], float out[3][3])
    {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out[r][c] = source[r][c];
            }
        }
    }

    void MatrixApplyComAxisCorrection(const float base[3][3], float angle, float out[3][3])
    {
        float axisX = base[0][0];
        float axisY = -base[0][1];
        float axisZ = -base[0][2];

        float correction[3][3];
        MatrixAxisAngleRadians(axisX, axisY, axisZ, angle, correction);
        MatrixMultiply3x3(base, correction, out);
    }

    void MatrixApplyLocalZPostCorrection(const float base[3][3], float angle, float out[3][3])
    {
        float correction[3][3];
        MatrixAxisAngleRadians(0.0f, 0.0f, 1.0f, angle, correction);
        MatrixMultiply3x3(base, correction, out);
    }

    bool MatrixFindComLocalZPostPreserveCorrection(const float source[3][3], const float base[3][3], float out[3][3])
    {
        const float targetTwist = MatrixLocalZTwistDegrees(source);
        float bestAngle = 0.0f;
        float bestError = 99999.0f;

        float candidate[3][3];
        for (int i = -180; i <= 180; ++i) {
            const float angle = static_cast<float>(i) / kRadiansToDegrees;
            MatrixApplyLocalZPostCorrection(base, angle, candidate);
            const float error = std::fabs(MatrixLocalZTwistDeltaDegrees(MatrixLocalZTwistDegrees(candidate), targetTwist));
            if (error < bestError) {
                bestError = error;
                bestAngle = angle;
            }
        }

        const float step = 0.05f / kRadiansToDegrees;
        const int refineSteps = 80;
        for (int i = -refineSteps; i <= refineSteps; ++i) {
            const float angle = bestAngle + static_cast<float>(i) * step;
            MatrixApplyLocalZPostCorrection(base, angle, candidate);
            const float error = std::fabs(MatrixLocalZTwistDeltaDegrees(MatrixLocalZTwistDegrees(candidate), targetTwist));
            if (error < bestError) {
                bestError = error;
                bestAngle = angle;
            }
        }

        MatrixApplyLocalZPostCorrection(base, bestAngle, out);
        return std::isfinite(bestError) && bestError < 2.5f;
    }

    void MatrixApplyComTwistUnwind(const float source[3][3], float mirrored[3][3])
    {
        float preserved[3][3];
        if (MatrixFindComLocalZPostPreserveCorrection(source, mirrored, preserved)) {
            CopyMatrix3x3(preserved, mirrored);
        }
    }

    bool NodeNameMatches(NiAVObject* object, const std::vector<std::string>& wantedNamesLower)
    {
        if (!object) {
            return false;
        }

        const char* objectNameRaw = object->m_name.c_str();
        if (!objectNameRaw || !objectNameRaw[0]) {
            return false;
        }

        const std::string objectNameLower = ToLowerAscii(objectNameRaw);
        for (const auto& wantedLower : wantedNamesLower) {
            if (objectNameLower == wantedLower) {
                return true;
            }
        }

        return false;
    }


    NiAVObject* FindNodeRecursiveGuarded(NiAVObject* object, const std::vector<std::string>& wantedNamesLower, std::vector<NiAVObject*>& visited, UInt32 depth, UInt32& visitedCount)
    {


        constexpr UInt32 kMaxDepth = 96;
        constexpr UInt32 kMaxVisited = 768;

        if (!object || depth > kMaxDepth || visitedCount >= kMaxVisited) {
            return nullptr;
        }

        if (std::find(visited.begin(), visited.end(), object) != visited.end()) {
            return nullptr;
        }
        visited.push_back(object);
        ++visitedCount;

        if (NodeNameMatches(object, wantedNamesLower)) {
            return object;
        }

        NiNode* node = object->GetAsNiNode();
        if (!node || !node->m_children.m_data) {
            return nullptr;
        }

        const UInt16 limit = node->m_children.m_emptyRunStart;
        for (UInt16 i = 0; i < limit; ++i) {
            NiAVObject* child = node->m_children.m_data[i];
            if (!child) {
                continue;
            }

            NiAVObject* found = FindNodeRecursiveGuarded(child, wantedNamesLower, visited, depth + 1, visitedCount);
            if (found) {
                return found;
            }
            if (visitedCount >= kMaxVisited) {
                return nullptr;
            }
        }

        return nullptr;
    }

    NiAVObject* FindNodeRecursive(NiAVObject* object, const std::vector<std::string>& wantedNamesLower)
    {
        std::vector<NiAVObject*> visited;
        visited.reserve(128);
        UInt32 visitedCount = 0;
        return FindNodeRecursiveGuarded(object, wantedNamesLower, visited, 0, visitedCount);
    }


    struct PoseNode
    {
        bool found = false;
        std::string requestedName;
        std::string matchedName;
        int rootIndex = -1;


        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float scale = 1.0f;
        float rot[3][3] = {
            { 1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f }
        };

        float yaw = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;
    };
    std::unordered_set<std::string> g_poseMirrorOperationState;


    std::vector<std::string> SplitPipeList(const std::string& text)
    {
        std::vector<std::string> result;
        std::string current;
        for (char c : text) {
            if (c == '|') {
                const std::string item = TrimAscii(current);
                if (!item.empty()) {
                    result.push_back(item);
                }
                current.clear();
            }
            else {
                current.push_back(c);
            }
        }
        const std::string item = TrimAscii(current);
        if (!item.empty()) {
            result.push_back(item);
        }
        return result;
    }

    std::string JsonEscape(const std::string& text)
    {
        std::string out;
        out.reserve(text.size() + 8);
        for (unsigned char c : text) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned int>(c));
                    out += buf;
                }
                else {
                    out.push_back(static_cast<char>(c));
                }
                break;
            }
        }
        return out;
    }

    std::string FormatFloat(float value)
    {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "%.6f", value);
        return std::string(buffer);
    }

    float NormalizeAngleDelta(float angleValue)
    {
        while (angleValue > 180.0f) {
            angleValue -= 360.0f;
        }
        while (angleValue < -180.0f) {
            angleValue += 360.0f;
        }
        return angleValue;
    }


    std::filesystem::path GetPoseMirrorPoseBuildErrorPath()
    {
        std::error_code ec;
        const auto dir = GetRuntimePluginDirectory();
        std::filesystem::create_directories(dir, ec);
        return dir / "Straw_PoseMirror_PlayIdle_PoseBuildError.json";
    }

    std::filesystem::path GetSafAdjustmentsDirectory()
    {
        std::error_code ec;
        const auto dir = GetRuntimePluginDirectory() / "SAF" / "Adjustments";
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::filesystem::path GetSafPosesDirectory()
    {
        std::error_code ec;
        const auto dir = GetRuntimePluginDirectory() / "SAF" / "Poses";
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    bool CleanupPoseMirrorTemporaryFilesInternal()
    {
        bool ok = true;
        try {
            std::error_code ec;
            const auto posesDir = GetSafPosesDirectory();
            std::filesystem::remove(posesDir / "Straw_PoseMirror_PlayIdle_SavePoseProbe.json", ec);
            if (ec) {
                ok = false;
                ec.clear();
            }
            std::filesystem::remove(posesDir / "Straw_PoseMirror_PlayIdle_MirroredPose.json", ec);
            if (ec) {
                ok = false;
                ec.clear();
            }
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
            if (ec) {
                ok = false;
            }
        }
        catch (...) {
            ok = false;
        }
        return ok;
    }

    bool IsSafeAdjustmentStem(const std::string& stem)
    {
        if (stem.empty() || stem.size() > 120) {
            return false;
        }
        if (stem == "." || stem == ".." || stem.find("..") != std::string::npos) {
            return false;
        }
        for (unsigned char ch : stem) {
            if (ch < 0x20) {
                return false;
            }
            switch (ch) {
            case '\\':
            case '/':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                return false;
            default:
                break;
            }
        }
        return true;
    }

    bool DeletePoseMirrorAdjustmentFileInternal(const std::string& stem)
    {
        if (!IsSafeAdjustmentStem(stem)) {
            return false;
        }

        try {
            std::error_code ec;
            const auto path = GetSafAdjustmentsDirectory() / (stem + ".json");
            if (!std::filesystem::exists(path, ec)) {
                return true;
            }
            ec.clear();
            return std::filesystem::remove(path, ec) && !ec;
        }
        catch (...) {
            return false;
        }
    }


    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
            return {};
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }


    bool ParseNextFloatToken(const std::string& text, std::size_t& pos, float& value)
    {
        while (pos < text.size()) {
            char c = text[pos];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
                char* endPtr = nullptr;
                value = std::strtof(text.c_str() + pos, &endPtr);
                if (endPtr && endPtr != text.c_str() + pos) {
                    pos = static_cast<std::size_t>(endPtr - text.c_str());
                    return true;
                }
                return false;
            }
            ++pos;
        }
        return false;
    }

    bool ExtractJsonFloatAfter(const std::string& text, std::size_t start, const std::string& key, float& value)
    {
        const std::string marker = "\"" + key + "\"";
        std::size_t pos = text.find(marker, start);
        if (pos == std::string::npos) {
            return false;
        }
        pos = text.find(':', pos + marker.size());
        if (pos == std::string::npos) {
            return false;
        }
        ++pos;
        return ParseNextFloatToken(text, pos, value);
    }


    void WriteBuildReport(const std::string& reason, TESObjectREFR* targetRef, int written, int missingSource, int missingDestination, int computeFailed);

    std::filesystem::path GetPoseMirrorSavedPoseProbePath()
    {
        std::error_code ec;
        const auto dir = GetSafPosesDirectory();
        std::filesystem::create_directories(dir, ec);
        return dir / "Straw_PoseMirror_PlayIdle_SavePoseProbe.json";
    }

    std::string ReadSavedPoseProbeText(std::filesystem::path& usedPath, std::vector<std::filesystem::path>& triedPaths)
    {
        triedPaths.clear();
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(GetPoseMirrorSavedPoseProbePath());
        candidates.push_back(GetPluginDirectory() / "SAF" / "Poses" / "Straw_PoseMirror_PlayIdle_SavePoseProbe.json");
        candidates.push_back(GetGameRootDirectory() / "Data" / "F4SE" / "Plugins" / "SAF" / "Poses" / "Straw_PoseMirror_PlayIdle_SavePoseProbe.json");

        for (const auto& candidate : candidates) {
            bool duplicate = false;
            for (const auto& existing : triedPaths) {
                std::error_code ec;
                if (std::filesystem::equivalent(existing, candidate, ec)) {
                    duplicate = true;
                    break;
                }
                if (!ec && existing == candidate) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            triedPaths.push_back(candidate);
            const std::string text = ReadTextFile(candidate);
            if (!text.empty()) {
                usedPath = candidate;
                return text;
            }
        }

        usedPath.clear();
        return {};
    }

    bool FindJsonObjectForKey(const std::string& text, const std::string& key, std::size_t& objectStart, std::size_t& objectEnd)
    {
        const std::string marker = "\"" + key + "\"";
        std::size_t keyPos = text.find(marker);
        if (keyPos == std::string::npos) {
            return false;
        }

        objectStart = text.find('{', keyPos + marker.size());
        if (objectStart == std::string::npos) {
            return false;
        }

        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (std::size_t i = objectStart; i < text.size(); ++i) {
            const char ch = text[i];

            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\' && inString) {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                inString = !inString;
                continue;
            }
            if (inString) {
                continue;
            }

            if (ch == '{') {
                ++depth;
            }
            else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    objectEnd = i;
                    return true;
                }
                if (depth < 0) {
                    return false;
                }
            }
        }

        return false;
    }

    bool ExtractSafPoseTransform(const std::string& text, const std::string& nodeName, PoseNode& data)
    {
        data = PoseNode{};
        data.requestedName = nodeName;
        data.matchedName = nodeName;

        std::size_t objectStart = 0;
        std::size_t objectEnd = 0;
        if (!FindJsonObjectForKey(text, nodeName, objectStart, objectEnd) || objectEnd <= objectStart) {
            return false;
        }

        const std::string chunk = text.substr(objectStart, objectEnd - objectStart + 1);

        bool ok = true;
        ok = ExtractJsonFloatAfter(chunk, 0, "x", data.posX) && ok;
        ok = ExtractJsonFloatAfter(chunk, 0, "y", data.posY) && ok;
        ok = ExtractJsonFloatAfter(chunk, 0, "z", data.posZ) && ok;
        ok = ExtractJsonFloatAfter(chunk, 0, "yaw", data.yaw) && ok;
        ok = ExtractJsonFloatAfter(chunk, 0, "pitch", data.pitch) && ok;
        ok = ExtractJsonFloatAfter(chunk, 0, "roll", data.roll) && ok;
        if (!ExtractJsonFloatAfter(chunk, 0, "scale", data.scale)) {
            data.scale = 1.0f;
        }

        if (!ok) {
            return false;
        }

        MatrixFromEulerXYZDegreesRaw(data.yaw, data.pitch, data.roll, data.rot);
        data.found = true;
        return true;
    }

    void WriteSafPoseTransformJson(std::ofstream& out, const std::string& nodeName, float x, float y, float z, float yaw, float pitch, float roll, float scale)
    {
        out << "    \"" << JsonEscape(nodeName) << "\": {\n";
        out << "      \"pitch\": \"" << FormatFloat(NormalizeAngleDelta(pitch)) << "\",\n";
        out << "      \"roll\": \"" << FormatFloat(NormalizeAngleDelta(roll)) << "\",\n";
        out << "      \"scale\": \"" << FormatFloat(scale) << "\",\n";
        out << "      \"x\": \"" << FormatFloat(x) << "\",\n";
        out << "      \"y\": \"" << FormatFloat(y) << "\",\n";
        out << "      \"yaw\": \"" << FormatFloat(NormalizeAngleDelta(yaw)) << "\",\n";
        out << "      \"z\": \"" << FormatFloat(z) << "\"\n";
        out << "    }";
    }


    void MatrixTranspose3x3(const float in[3][3], float out[3][3])
    {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out[r][c] = in[c][r];
            }
        }
    }

    void MatrixRotateVector(const float matrix[3][3], float x, float y, float z, float& outX, float& outY, float& outZ)
    {
        outX = matrix[0][0] * x + matrix[0][1] * y + matrix[0][2] * z;
        outY = matrix[1][0] * x + matrix[1][1] * y + matrix[1][2] * z;
        outZ = matrix[2][0] * x + matrix[2][1] * y + matrix[2][2] * z;
    }

    PoseNode InvertPoseNode(const PoseNode& src)
    {
        PoseNode out;
        out.scale = (std::fabs(src.scale) <= 0.000001f) ? 1.0f : 1.0f / src.scale;
        MatrixTranspose3x3(src.rot, out.rot);
        MatrixRotateVector(out.rot, -src.posX / out.scale, -src.posY / out.scale, -src.posZ / out.scale, out.posX, out.posY, out.posZ);
        MatrixToEulerXYZDegreesRaw(out.rot, out.yaw, out.pitch, out.roll);
        out.found = true;
        return out;
    }

    PoseNode MultiplyPoseNode(const PoseNode& lhs, const PoseNode& rhs)
    {
        PoseNode out;
        out.scale = lhs.scale * rhs.scale;
        MatrixMultiply3x3(lhs.rot, rhs.rot, out.rot);
        float rx = 0.0f;
        float ry = 0.0f;
        float rz = 0.0f;
        MatrixRotateVector(lhs.rot, rhs.posX, rhs.posY, rhs.posZ, rx, ry, rz);
        out.posX = lhs.posX + rx * lhs.scale;
        out.posY = lhs.posY + ry * lhs.scale;
        out.posZ = lhs.posZ + rz * lhs.scale;
        MatrixToEulerXYZDegreesRaw(out.rot, out.yaw, out.pitch, out.roll);
        out.found = true;
        return out;
    }

    PoseNode IdentityPoseNode()
    {
        PoseNode out;
        out.found = true;
        return out;
    }


    bool WasNodeAlreadyWritten(const std::vector<std::string>& writtenNodes, const std::string& nodeName)
    {
        for (const auto& existing : writtenNodes) {
            if (existing == nodeName) {
                return true;
            }
        }
        return false;
    }


    bool ParseJsonStringLiteral(const std::string& text, std::size_t quotePos, std::string& value, std::size_t& afterQuote)
    {
        value.clear();
        afterQuote = quotePos;
        if (quotePos >= text.size() || text[quotePos] != '"') {
            return false;
        }

        bool escaped = false;
        for (std::size_t i = quotePos + 1; i < text.size(); ++i) {
            const char ch = text[i];
            if (escaped) {


                value.push_back(ch);
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                afterQuote = i + 1;
                return true;
            }
            value.push_back(ch);
        }
        return false;
    }

    std::vector<std::string> ExtractSafPoseTransformKeys(const std::string& text)
    {
        std::vector<std::string> keys;

        const std::string marker = "\"transforms\"";
        const std::size_t markerPos = text.find(marker);
        if (markerPos == std::string::npos) {
            return keys;
        }

        const std::size_t objectStart = text.find('{', markerPos + marker.size());
        if (objectStart == std::string::npos) {
            return keys;
        }

        int depth = 0;
        bool inString = false;
        bool escaped = false;
        std::size_t objectEnd = std::string::npos;
        for (std::size_t i = objectStart; i < text.size(); ++i) {
            const char ch = text[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\' && inString) {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                inString = !inString;
                continue;
            }
            if (inString) {
                continue;
            }
            if (ch == '{') {
                ++depth;
            }
            else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    objectEnd = i;
                    break;
                }
            }
        }

        if (objectEnd == std::string::npos || objectEnd <= objectStart) {
            return keys;
        }

        std::size_t i = objectStart + 1;
        while (i < objectEnd) {
            while (i < objectEnd && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',')) {
                ++i;
            }
            if (i >= objectEnd) {
                break;
            }
            if (text[i] != '"') {
                ++i;
                continue;
            }

            std::string key;
            std::size_t afterKey = i;
            if (!ParseJsonStringLiteral(text, i, key, afterKey)) {
                break;
            }

            std::size_t colon = afterKey;
            while (colon < objectEnd && std::isspace(static_cast<unsigned char>(text[colon]))) {
                ++colon;
            }
            if (colon >= objectEnd || text[colon] != ':') {
                i = afterKey;
                continue;
            }

            std::size_t valueStart = colon + 1;
            while (valueStart < objectEnd && std::isspace(static_cast<unsigned char>(text[valueStart]))) {
                ++valueStart;
            }
            if (valueStart >= objectEnd || text[valueStart] != '{') {
                i = valueStart + 1;
                continue;
            }

            keys.push_back(key);

            int valueDepth = 0;
            bool valueInString = false;
            bool valueEscaped = false;
            std::size_t valueEnd = valueStart;
            for (; valueEnd < objectEnd; ++valueEnd) {
                const char ch = text[valueEnd];
                if (valueEscaped) {
                    valueEscaped = false;
                    continue;
                }
                if (ch == '\\' && valueInString) {
                    valueEscaped = true;
                    continue;
                }
                if (ch == '"') {
                    valueInString = !valueInString;
                    continue;
                }
                if (valueInString) {
                    continue;
                }
                if (ch == '{') {
                    ++valueDepth;
                }
                else if (ch == '}') {
                    --valueDepth;
                    if (valueDepth == 0) {
                        ++valueEnd;
                        break;
                    }
                }
            }
            i = valueEnd;
        }

        return keys;
    }

    bool ReplacePrefixMirror(const std::string& input, const std::string& leftPrefix, const std::string& rightPrefix, std::string& output)
    {
        if (input.rfind(leftPrefix, 0) == 0) {
            output = rightPrefix + input.substr(leftPrefix.size());
            return true;
        }
        if (input.rfind(rightPrefix, 0) == 0) {
            output = leftPrefix + input.substr(rightPrefix.size());
            return true;
        }
        return false;
    }

    bool ReplaceFirstTokenMirror(const std::string& input, const std::string& leftToken, const std::string& rightToken, std::string& output)
    {
        const std::size_t leftPos = input.find(leftToken);
        const std::size_t rightPos = input.find(rightToken);

        if (leftPos == std::string::npos && rightPos == std::string::npos) {
            return false;
        }
        if (leftPos != std::string::npos && (rightPos == std::string::npos || leftPos < rightPos)) {
            output = input;
            output.replace(leftPos, leftToken.size(), rightToken);
            return true;
        }
        output = input;
        output.replace(rightPos, rightToken.size(), leftToken);
        return true;
    }

    std::string GetSidewaysMirroredNodeName(const std::string& nodeName)
    {
        std::string mirrored;
        if (ReplacePrefixMirror(nodeName, "LArm_", "RArm_", mirrored)) return mirrored;
        if (ReplacePrefixMirror(nodeName, "LLeg_", "RLeg_", mirrored)) return mirrored;
        if (ReplacePrefixMirror(nodeName, "LBreast", "RBreast", mirrored)) return mirrored;
        if (ReplacePrefixMirror(nodeName, "LButt", "RButt", mirrored)) return mirrored;
        if (ReplacePrefixMirror(nodeName, "L_Rib", "R_Rib", mirrored)) return mirrored;


        if (ReplaceFirstTokenMirror(nodeName, "_L_", "_R_", mirrored)) return mirrored;
        if (ReplaceFirstTokenMirror(nodeName, "_Left_", "_Right_", mirrored)) return mirrored;

        return nodeName;
    }

    bool IsRootOrAttachmentNodeForFullPoseMirror(const std::string& nodeName)
    {
        const std::string lower = ToLowerAscii(nodeName);





        if (lower.rfind("animobject", 0) == 0) {
            return true;
        }
        if (lower == "weapon" || lower == "weaponleft" || lower == "pipboybone") {
            return true;
        }

        return false;
    }

    bool WriteFullMirroredSavedPoseNode(std::ofstream& out, const std::string& text, const std::string& srcName, const std::string& dstName, bool isSidePair, std::vector<std::string>& writtenNodes, int& missingSource, int& computeFailed, int& written)
    {



        if (IsRootOrAttachmentNodeForFullPoseMirror(srcName)) {
            return true;
        }

        if (WasNodeAlreadyWritten(writtenNodes, dstName)) {
            return true;
        }

        PoseNode srcData;
        if (!ExtractSafPoseTransform(text, srcName, srcData)) {
            ++missingSource;
            return false;
        }

        float outX = srcData.posX;
        float outY = srcData.posY;
        float outZ = srcData.posZ;

        float mirroredRot[3][3];
        if (ToLowerAscii(srcName) == "com") {
            MatrixMirrorWorldZLocalX(srcData.rot, mirroredRot);
            MatrixApplyComTwistUnwind(srcData.rot, mirroredRot);
        }
        else {
            MatrixMirrorLocalZ(srcData.rot, mirroredRot);
        }

        if (!std::isfinite(mirroredRot[0][0])) {
            ++computeFailed;
            return false;
        }

        float yaw = srcData.yaw;
        float pitch = srcData.pitch;
        float roll = srcData.roll;
        MatrixToEulerXYZDegreesRaw(mirroredRot, yaw, pitch, roll);





        if (isSidePair) {
            outZ = -srcData.posZ;
        }

        if (written > 0) {
            out << ",\n";
        }

        WriteSafPoseTransformJson(out, dstName, outX, outY, outZ, yaw, pitch, roll, srcData.scale);
        writtenNodes.push_back(dstName);
        ++written;
        return true;
    }


    bool BuildFullPoseMirrorFromSavedPoseFileInternal(TESObjectREFR* targetRef, const std::string& filename)
    {

        {
            std::error_code ec;
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
        }

        if (!targetRef) {
            WriteBuildReport("full saved-pose mirror failed: invalid target", targetRef, 0, 0, 0, 0);
            return false;
        }
        if (!IsSafeAdjustmentStem(filename)) {
            WriteBuildReport("full saved-pose mirror failed: unsafe output filename", targetRef, 0, 0, 0, 0);
            return false;
        }

        std::filesystem::path inputPath;
        std::vector<std::filesystem::path> triedInputPaths;
        const std::string text = ReadSavedPoseProbeText(inputPath, triedInputPaths);
        if (text.empty()) {
            WriteBuildReport("full saved-pose mirror failed: SavePose probe file missing or empty in all candidate paths", targetRef, 0, 0, 0, 0);
            return false;
        }

        const std::vector<std::string> sourceNodeNames = ExtractSafPoseTransformKeys(text);
        if (sourceNodeNames.empty()) {
            WriteBuildReport("full saved-pose mirror failed: no transform entries found in SavePose probe", targetRef, 0, 0, 0, 0);
            return false;
        }

        try {
            const auto outputPath = GetSafPosesDirectory() / (filename + ".json");
            std::ofstream out(outputPath, std::ios::trunc);
            if (!out) {
                WriteBuildReport("full saved-pose mirror failed: could not open output pose file", targetRef, 0, 0, 0, 0);
                return false;
            }

            out << "{\n";
            out << "  \"name\": \"" << JsonEscape(filename) << "\",\n";
            out << "  \"skeleton\": \"All\",\n";
            out << "  \"transforms\": {\n";

            int written = 0;
            int missingSource = 0;
            int computeFailed = 0;
            std::vector<std::string> writtenNodes;

            for (const auto& sourceName : sourceNodeNames) {
                if (sourceName.empty()) {
                    ++computeFailed;
                    continue;
                }
                const std::string destinationName = GetSidewaysMirroredNodeName(sourceName);
                const bool isSidePair = (destinationName != sourceName);
                WriteFullMirroredSavedPoseNode(out, text, sourceName, destinationName, isSidePair, writtenNodes, missingSource, computeFailed, written);
            }

            out << "\n  },\n";
            out << "  \"version\": 2\n";
            out << "}\n";

            if (written <= 0) {
                WriteBuildReport("full saved-pose mirror failed: zero transforms written from SavePose probe", targetRef, written, missingSource, 0, computeFailed);
                return false;
            }

            return true;
        }
        catch (...) {
            WriteBuildReport("full saved-pose mirror failed: exception while writing output pose file", targetRef, 0, 0, 0, 0);
            return false;
        }
    }

    bool BuildPoseMirrorFromSavedPoseFileInternal(TESObjectREFR* targetRef, const std::vector<std::string>& sourceNodes, const std::vector<std::string>& destinationNodes, const std::string& filename, SInt32 mode)
    {
        {
            std::error_code ec;
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
        }

        if (!targetRef || sourceNodes.empty() || destinationNodes.empty() || destinationNodes.size() < sourceNodes.size()) {
            WriteBuildReport("saved-pose mirror failed: invalid input or bone list", targetRef, 0, 0, 0, 0);
            return false;
        }
        if (!IsSafeAdjustmentStem(filename)) {
            WriteBuildReport("saved-pose mirror failed: unsafe output filename", targetRef, 0, 0, 0, 0);
            return false;
        }

        std::filesystem::path inputPath;
        std::vector<std::filesystem::path> triedInputPaths;
        const std::string text = ReadSavedPoseProbeText(inputPath, triedInputPaths);
        if (text.empty()) {
            WriteBuildReport("saved-pose mirror failed: SavePose probe file missing or empty in all candidate paths", targetRef, 0, 0, 0, 0);
            return false;
        }

        try {
            const auto outputPath = GetSafPosesDirectory() / (filename + ".json");
            std::ofstream out(outputPath, std::ios::trunc);
            if (!out) {
                WriteBuildReport("saved-pose mirror failed: could not open output pose file", targetRef, 0, 0, 0, 0);
                return false;
            }

            out << "{\n";
            out << "  \"name\": \"" << JsonEscape(filename) << "\",\n";
            out << "  \"skeleton\": \"All\",\n";
            out << "  \"transforms\": {\n";

            int written = 0;
            int missingSource = 0;
            int missingDestination = 0;
            int computeFailed = 0;

            for (std::size_t i = 0; i < sourceNodes.size(); ++i) {
                const std::string& srcName = sourceNodes[i];
                const std::string& dstName = destinationNodes[i];

                PoseNode srcData;
                if (!ExtractSafPoseTransform(text, srcName, srcData)) {
                    ++missingSource;
                    continue;
                }

                PoseNode dstData;
                const bool haveDestination = ExtractSafPoseTransform(text, dstName, dstData);
                if (!haveDestination) {
                    ++missingDestination;
                }

                float mirroredRot[3][3];
                MatrixMirrorLocalZ(srcData.rot, mirroredRot);

                float yaw = 0.0f;
                float pitch = 0.0f;
                float roll = 0.0f;
                MatrixToEulerXYZDegreesRaw(mirroredRot, yaw, pitch, roll);



                float outX = srcData.posX;
                float outY = srcData.posY;
                float outZ = -srcData.posZ;
                if (mode == 61 && haveDestination) {
                    outX = dstData.posX;
                    outY = dstData.posY;
                    outZ = dstData.posZ;
                }

                if (written > 0) {
                    out << ",\n";
                }
                WriteSafPoseTransformJson(out, dstName, outX, outY, outZ, yaw, pitch, roll, srcData.scale);
                ++written;
            }

            out << "\n  },\n";
            out << "  \"version\": 2\n";
            out << "}\n";

            if (written <= 0) {
                WriteBuildReport("saved-pose mirror failed: zero transforms written from SavePose probe", targetRef, written, missingSource, missingDestination, computeFailed);
                return false;
            }

            return true;
        }
        catch (...) {
            WriteBuildReport("saved-pose mirror failed: exception while writing output pose file", targetRef, 0, 0, 0, 0);
            return false;
        }
    }


    std::filesystem::path GetPoseClipboardSavedPoseProbePath()
    {
        std::error_code ec;
        const auto dir = GetSafPosesDirectory();
        std::filesystem::create_directories(dir, ec);
        return dir / "Straw_PoseClipboard_SavePoseProbe.json";
    }

    std::filesystem::path GetPoseClipboardPastePosePath()
    {
        std::error_code ec;
        const auto dir = GetSafPosesDirectory();
        std::filesystem::create_directories(dir, ec);
        return dir / "Straw_PoseClipboard_PastePose.json";
    }

    std::filesystem::path GetPoseClipboardPath()
    {
        std::error_code ec;
        const auto dir = GetRuntimePluginDirectory();
        std::filesystem::create_directories(dir, ec);
        return dir / "Straw_PoseClipboard_PlayIdle.json";
    }

    std::filesystem::path GetPoseClipboardSamPath()
    {
        std::error_code ec;
        const auto dir = GetRuntimePluginDirectory();
        std::filesystem::create_directories(dir, ec);
        return dir / "Straw_PoseClipboard_SamPose.json";
    }

    std::filesystem::path GetPoseClipboardSamSourceAdjustmentPath()
    {
        std::error_code ec;
        const auto dir = GetSafAdjustmentsDirectory();
        std::filesystem::create_directories(dir, ec);
        return dir / "Straw_PoseClipboard_SamSourceAdjustment.json";
    }

    std::filesystem::path GetPoseClipboardSamDestinationAdjustmentPath()
    {
        std::error_code ec;
        const auto dir = GetSafAdjustmentsDirectory();
        std::filesystem::create_directories(dir, ec);
        return dir / "Straw_PoseClipboard_SamDestinationAdjustment.json";
    }

    std::filesystem::path GetPoseClipboardActiveAdjustmentPath()
    {
        std::error_code ec;
        const auto dir = GetSafAdjustmentsDirectory();
        std::filesystem::create_directories(dir, ec);
        return dir / "CopyPastePoseLimbs Data.json";
    }

    std::string ReadFirstExistingText(const std::vector<std::filesystem::path>& candidates, std::filesystem::path& usedPath, std::vector<std::filesystem::path>& triedPaths)
    {
        triedPaths.clear();
        for (const auto& candidate : candidates) {
            bool duplicate = false;
            for (const auto& existing : triedPaths) {
                std::error_code ec;
                if (std::filesystem::equivalent(existing, candidate, ec)) {
                    duplicate = true;
                    break;
                }
                if (!ec && existing == candidate) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            triedPaths.push_back(candidate);
            const std::string text = ReadTextFile(candidate);
            if (!text.empty()) {
                usedPath = candidate;
                return text;
            }
        }

        usedPath.clear();
        return {};
    }

    std::string ReadPoseClipboardSavedPoseText(std::filesystem::path& usedPath, std::vector<std::filesystem::path>& triedPaths)
    {
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(GetPoseClipboardSavedPoseProbePath());
        candidates.push_back(GetPluginDirectory() / "SAF" / "Poses" / "Straw_PoseClipboard_SavePoseProbe.json");
        candidates.push_back(GetGameRootDirectory() / "Data" / "F4SE" / "Plugins" / "SAF" / "Poses" / "Straw_PoseClipboard_SavePoseProbe.json");
        return ReadFirstExistingText(candidates, usedPath, triedPaths);
    }

    std::string ReadPoseClipboardSavedAdjustmentText(std::filesystem::path& usedPath, std::vector<std::filesystem::path>& triedPaths)
    {
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(GetPoseClipboardSamSourceAdjustmentPath());
        candidates.push_back(GetPluginDirectory() / "SAF" / "Adjustments" / "Straw_PoseClipboard_SamSourceAdjustment.json");
        candidates.push_back(GetGameRootDirectory() / "Data" / "F4SE" / "Plugins" / "SAF" / "Adjustments" / "Straw_PoseClipboard_SamSourceAdjustment.json");
        return ReadFirstExistingText(candidates, usedPath, triedPaths);
    }

    std::string ReadPoseClipboardDestinationAdjustmentText(std::filesystem::path& usedPath, std::vector<std::filesystem::path>& triedPaths)
    {
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(GetPoseClipboardSamDestinationAdjustmentPath());
        candidates.push_back(GetPluginDirectory() / "SAF" / "Adjustments" / "Straw_PoseClipboard_SamDestinationAdjustment.json");
        candidates.push_back(GetGameRootDirectory() / "Data" / "F4SE" / "Plugins" / "SAF" / "Adjustments" / "Straw_PoseClipboard_SamDestinationAdjustment.json");
        return ReadFirstExistingText(candidates, usedPath, triedPaths);
    }

    bool CleanupPoseClipboardTemporaryFilesInternal()
    {
        bool ok = true;
        try {
            std::error_code ec;
            std::filesystem::remove(GetPoseClipboardSavedPoseProbePath(), ec);
            if (ec) {
                ok = false;
                ec.clear();
            }
            std::filesystem::remove(GetPoseClipboardPastePosePath(), ec);
            if (ec) {
                ok = false;
                ec.clear();
            }
            std::filesystem::remove(GetPoseClipboardSamSourceAdjustmentPath(), ec);
            if (ec) {
                ok = false;
                ec.clear();
            }
            std::filesystem::remove(GetPoseClipboardSamDestinationAdjustmentPath(), ec);
            if (ec) {
                ok = false;
                ec.clear();
            }
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
            if (ec) {
                ok = false;
            }
        }
        catch (...) {
            ok = false;
        }
        return ok;
    }

    bool DeletePoseClipboardFileInternal(const std::filesystem::path& clipboardPath)
    {
        bool ok = CleanupPoseClipboardTemporaryFilesInternal();
        try {
            std::error_code ec;
            std::filesystem::remove(clipboardPath, ec);
            if (ec) {
                ok = false;
                ec.clear();
            }
        }
        catch (...) {
            ok = false;
        }
        return ok;
    }

    bool DeletePoseClipboardFilesInternal()
    {
        return DeletePoseClipboardFileInternal(GetPoseClipboardPath());
    }

    bool DeletePoseClipboardSamFilesInternal()
    {
        return DeletePoseClipboardFileInternal(GetPoseClipboardSamPath());
    }

    void AddUniqueText(std::vector<std::string>& values, const std::string& value)
    {
        if (value.empty()) {
            return;
        }
        for (const auto& existing : values) {
            if (existing == value) {
                return;
            }
        }
        values.push_back(value);
    }

    std::vector<std::string> GetPoseClipboardTorsoHeadPresetNodes()
    {
        return {
            "Spine",
            "Spine1",
            "Spine2",
            "Spine1_skin",
            "Spine1_skin_Offset",
            "Spine1_Rear_skin",
            "Spine1_Rear_skin_Offset",
            "Spine2_skin",
            "Spine2_skin_Offset",
            "Spine2_Rear_skin",
            "Spine2_Rear_skin_Offset",
            "Chest",
            "Chest_Offset",
            "Chest_skin",
            "Chest_skin_Offset",
            "Chest_Rear_Skin",
            "Chest_Rear_Skin_Offset",
            "Chest_Upper_skin",
            "Chest_Upper_skin_Offset",
            "Belly_01",
            "Belly_01_Offset",
            "Belly_02",
            "Belly_02_Offset",
            "Belly_skin",
            "Belly_skin_Offset",
            "UpperBelly_skin",
            "UpperBelly_skin_Offset",
            "L_RibHelper",
            "L_RibHelper_Offset",
            "R_RibHelper",
            "R_RibHelper_Offset",
            "Neck_Low_skin",
            "Neck_Low_skin_Offset",
            "Neck",
            "Neck_Offset",
            "Neck_skin",
            "Neck_skin_Offset",
            "Neck1",
            "Neck1_skin",
            "Neck1_skin_Offset",
            "Head",
            "Head_skin",
            "Head_skin_Offset",
            "Face_skin",
            "Face_skin_Offset",
            "Extra_Head",
            "Extra_Head_Offset",
            "Extra_Head_00",
            "Extra_Head_00_Offset",
            "Extra_Head_01",
            "Extra_Head_01_Offset",
            "Extra_Head_02",
            "Extra_Head_02_Offset",
            "Extra_Head_03",
            "Extra_Head_03_Offset",
            "Extra_Head_04",
            "Extra_Head_04_Offset",
            "Extra_Head_05",
            "Extra_Head_05_Offset",
            "Extra_Head_06",
            "Extra_Head_06_Offset",
            "Extra_Head_07",
            "Extra_Head_07_Offset",
            "Extra_Head_08",
            "Extra_Head_08_Offset",
            "Extra_Head_09",
            "Extra_Head_09_Offset",
            "Tongue_00",
            "Tongue_00_Offset",
            "Tongue_00_skin",
            "Tongue_00_skin_Offset",
            "Tongue_01",
            "Tongue_01_Offset",
            "Tongue_01_skin",
            "Tongue_01_skin_Offset",
            "Tongue_02",
            "Tongue_02_Offset",
            "Tongue_02_skin",
            "Tongue_02_skin_Offset",
            "Tongue_03",
            "Tongue_03_Offset",
            "Tongue_03_skin",
            "Tongue_03_skin_Offset",
            "Tongue_04",
            "Tongue_04_Offset",
            "Tongue_04_skin",
            "Tongue_04_skin_Offset",
            "Breast_CBP_L_00",
            "Breast_CBP_L_00_Offset",
            "Breast_CBP_L_01",
            "Breast_CBP_L_01_Offset",
            "Breast_CBP_L_02",
            "Breast_CBP_L_02_Offset",
            "Breast_CBP_L_03",
            "Breast_CBP_L_03_Offset",
            "Breast_CBP_L_03_OFFSET",
            "Breast_CBP_L_04",
            "Breast_CBP_L_04_Offset",
            "Breast_CBP_L_04_OFFSET",
            "Breast_CBP_R_00",
            "Breast_CBP_R_00_Offset",
            "Breast_CBP_R_01",
            "Breast_CBP_R_01_Offset",
            "Breast_CBP_R_02",
            "Breast_CBP_R_02_Offset",
            "Breast_CBP_R_03",
            "Breast_CBP_R_03_Offset",
            "Breast_CBP_R_04",
            "Breast_CBP_R_04_Offset",
            "Breast_L_00",
            "Breast_L_00_Offset",
            "Breast_L_01",
            "Breast_L_01_Offset",
            "Breast_L_02",
            "Breast_L_02_Offset",
            "Breast_R_00",
            "Breast_R_00_Offset",
            "Breast_R_01",
            "Breast_R_01_Offset",
            "Breast_R_02",
            "Breast_R_02_Offset",
            "LBreast_skin",
            "LBreast_skin_Offset",
            "LBreast_Offset",
            "LBreast_00_3B",
            "LBreast_00_3B_Offset",
            "LBreast_01_skin",
            "LBreast_01_skin_Offset",
            "LBreast_02_skin",
            "LBreast_02_skin_Offset",
            "LBreast_03_skin",
            "LBreast_03_skin_Offset",
            "RBreast_skin",
            "RBreast_skin_Offset",
            "RBreast_Offset",
            "RBreast_00_3B",
            "RBreast_00_3B_Offset",
            "RBreast_01_skin",
            "RBreast_01_skin_Offset",
            "RBreast_02_skin",
            "RBreast_02_skin_Offset",
            "RBreast_03_skin",
            "RBreast_03_skin_Offset"
        };
    }

    std::vector<std::string> ExpandPoseClipboardNodeList(const std::vector<std::string>& rawNodes)
    {
        std::vector<std::string> expanded;
        for (const auto& nodeName : rawNodes) {
            if (nodeName == "__TORSO_HEAD__") {
                const auto presetNodes = GetPoseClipboardTorsoHeadPresetNodes();
                for (const auto& presetNode : presetNodes) {
                    AddUniqueText(expanded, presetNode);
                }
            }
            else {
                AddUniqueText(expanded, nodeName);
            }
        }
        return expanded;
    }

    void ExtractPoseClipboardLabels(const std::string& text, std::vector<std::string>& labels)
    {
        labels.clear();

        const std::string arrayMarker = "\"clipboardLabels\"";
        std::size_t arrayPos = text.find(arrayMarker);
        if (arrayPos != std::string::npos) {
            std::size_t bracketOpen = text.find('[', arrayPos + arrayMarker.size());
            std::size_t bracketClose = std::string::npos;
            if (bracketOpen != std::string::npos) {
                bool inString = false;
                bool escaped = false;
                for (std::size_t i = bracketOpen + 1; i < text.size(); ++i) {
                    const char ch = text[i];
                    if (escaped) {
                        escaped = false;
                        continue;
                    }
                    if (ch == '\\' && inString) {
                        escaped = true;
                        continue;
                    }
                    if (ch == '"') {
                        inString = !inString;
                        continue;
                    }
                    if (!inString && ch == ']') {
                        bracketClose = i;
                        break;
                    }
                }
            }
            if (bracketOpen != std::string::npos && bracketClose != std::string::npos && bracketClose > bracketOpen) {
                std::size_t i = bracketOpen + 1;
                while (i < bracketClose) {
                    if (text[i] == '"') {
                        std::string label;
                        std::size_t afterQuote = i;
                        if (ParseJsonStringLiteral(text, i, label, afterQuote)) {
                            AddUniqueText(labels, label);
                            i = afterQuote;
                            continue;
                        }
                    }
                    ++i;
                }
            }
        }

        if (labels.empty()) {
            const std::string singleMarker = "\"clipboardLabel\"";
            std::size_t singlePos = text.find(singleMarker);
            if (singlePos != std::string::npos) {
                std::size_t colon = text.find(':', singlePos + singleMarker.size());
                if (colon != std::string::npos) {
                    std::size_t quote = text.find('"', colon + 1);
                    if (quote != std::string::npos) {
                        std::string label;
                        std::size_t afterQuote = quote;
                        if (ParseJsonStringLiteral(text, quote, label, afterQuote)) {
                            AddUniqueText(labels, label);
                        }
                    }
                }
            }
        }
    }

    int GetPoseClipboardLimbCountInternal(const std::filesystem::path& clipboardPath)
    {
        const std::string text = ReadTextFile(clipboardPath);
        if (text.empty()) {
            return 0;
        }

        const std::vector<std::string> nodes = ExtractSafPoseTransformKeys(text);
        if (nodes.empty()) {
            return 0;
        }

        std::vector<std::string> labels;
        ExtractPoseClipboardLabels(text, labels);
        if (!labels.empty()) {
            return static_cast<int>(labels.size());
        }

        return 1;
    }

    int GetPoseClipboardLimbCountInternal()
    {
        return GetPoseClipboardLimbCountInternal(GetPoseClipboardPath());
    }

    int GetPoseClipboardSamLimbCountInternal()
    {
        return GetPoseClipboardLimbCountInternal(GetPoseClipboardSamPath());
    }

    bool HasPoseClipboardInternal()
    {
        return GetPoseClipboardLimbCountInternal() > 0;
    }

    bool HasPoseClipboardSamInternal()
    {
        return GetPoseClipboardSamLimbCountInternal() > 0;
    }

    bool WritePoseClipboardPoseFile(const std::string& inputText, const std::vector<std::string>& nodes, const std::string& filename, const std::filesystem::path& outputPath, const std::string& label, TESObjectREFR* targetRef, const std::string& failurePrefix)
    {
        if (nodes.empty()) {
            WriteBuildReport(failurePrefix + ": no node list", targetRef, 0, 0, 0, 0);
            return false;
        }

        try {
            std::ofstream out(outputPath, std::ios::trunc);
            if (!out) {
                WriteBuildReport(failurePrefix + ": could not open output file", targetRef, 0, 0, 0, 0);
                return false;
            }

            out << "{\n";
            out << "  \"name\": \"" << JsonEscape(filename) << "\",\n";
            out << "  \"skeleton\": \"All\",\n";
            if (!label.empty()) {
                out << "  \"clipboardLabel\": \"" << JsonEscape(label) << "\",\n";
            }
            out << "  \"transforms\": {\n";

            int written = 0;
            int missingSource = 0;
            int computeFailed = 0;
            std::vector<std::string> writtenNodes;

            for (const auto& nodeName : nodes) {
                if (nodeName.empty() || WasNodeAlreadyWritten(writtenNodes, nodeName)) {
                    ++computeFailed;
                    continue;
                }

                PoseNode data;
                if (!ExtractSafPoseTransform(inputText, nodeName, data)) {
                    ++missingSource;
                    continue;
                }

                if (written > 0) {
                    out << ",\n";
                }
                WriteSafPoseTransformJson(out, nodeName, data.posX, data.posY, data.posZ, data.yaw, data.pitch, data.roll, data.scale);
                writtenNodes.push_back(nodeName);
                ++written;
            }

            out << "\n  },\n";
            out << "  \"version\": 2\n";
            out << "}\n";

            if (written <= 0) {
                WriteBuildReport(failurePrefix + ": zero transforms written", targetRef, written, missingSource, 0, computeFailed);
                return false;
            }

            return true;
        }
        catch (...) {
            WriteBuildReport(failurePrefix + ": exception while writing output file", targetRef, 0, 0, 0, 0);
            return false;
        }
    }

    bool IsNodeInList(const std::vector<std::string>& nodes, const std::string& nodeName)
    {
        for (const auto& node : nodes) {
            if (node == nodeName) {
                return true;
            }
        }
        return false;
    }

    bool WritePoseClipboardMergedFile(const std::string& existingText, const std::string& sourceText, const std::vector<std::string>& sourceNodes, const std::string& filename, const std::filesystem::path& outputPath, const std::string& label, TESObjectREFR* targetRef, const std::string& failurePrefix, bool fillMissingSourceWithIdentity)
    {
        if (sourceNodes.empty()) {
            WriteBuildReport(failurePrefix + ": no node list", targetRef, 0, 0, 0, 0);
            return false;
        }

        bool hasNewSourceData = false;
        for (const auto& nodeName : sourceNodes) {
            PoseNode data;
            if (!nodeName.empty() && ExtractSafPoseTransform(sourceText, nodeName, data)) {
                hasNewSourceData = true;
                break;
            }
        }
        if (!hasNewSourceData && !fillMissingSourceWithIdentity) {
            WriteBuildReport(failurePrefix + ": zero copied section transforms found", targetRef, 0, static_cast<int>(sourceNodes.size()), 0, 0);
            return false;
        }

        try {
            std::ofstream out(outputPath, std::ios::trunc);
            if (!out) {
                WriteBuildReport(failurePrefix + ": could not open output file", targetRef, 0, 0, 0, 0);
                return false;
            }

            std::vector<std::string> labels;
            ExtractPoseClipboardLabels(existingText, labels);
            AddUniqueText(labels, label);

            out << "{\n";
            out << "  \"name\": \"" << JsonEscape(filename) << "\",\n";
            out << "  \"skeleton\": \"All\",\n";
            if (!labels.empty()) {
                out << "  \"clipboardLabels\": [";
                for (std::size_t i = 0; i < labels.size(); ++i) {
                    if (i > 0) {
                        out << ", ";
                    }
                    out << "\"" << JsonEscape(labels[i]) << "\"";
                }
                out << "],\n";
            }
            out << "  \"transforms\": {\n";

            int written = 0;
            int missingSource = 0;
            int computeFailed = 0;
            std::vector<std::string> writtenNodes;

            const std::vector<std::string> existingNodes = ExtractSafPoseTransformKeys(existingText);
            for (const auto& nodeName : existingNodes) {
                if (nodeName.empty() || IsNodeInList(sourceNodes, nodeName) || WasNodeAlreadyWritten(writtenNodes, nodeName)) {
                    continue;
                }

                PoseNode data;
                if (!ExtractSafPoseTransform(existingText, nodeName, data)) {
                    ++computeFailed;
                    continue;
                }

                if (written > 0) {
                    out << ",\n";
                }
                WriteSafPoseTransformJson(out, nodeName, data.posX, data.posY, data.posZ, data.yaw, data.pitch, data.roll, data.scale);
                writtenNodes.push_back(nodeName);
                ++written;
            }

            for (const auto& nodeName : sourceNodes) {
                if (nodeName.empty() || WasNodeAlreadyWritten(writtenNodes, nodeName)) {
                    ++computeFailed;
                    continue;
                }

                PoseNode data;
                if (!ExtractSafPoseTransform(sourceText, nodeName, data)) {
                    if (!fillMissingSourceWithIdentity) {
                        ++missingSource;
                        continue;
                    }
                    data.posX = 0.0;
                    data.posY = 0.0;
                    data.posZ = 0.0;
                    data.yaw = 0.0;
                    data.pitch = 0.0;
                    data.roll = 0.0;
                    data.scale = 1.0;
                    ++missingSource;
                }

                if (written > 0) {
                    out << ",\n";
                }
                WriteSafPoseTransformJson(out, nodeName, data.posX, data.posY, data.posZ, data.yaw, data.pitch, data.roll, data.scale);
                writtenNodes.push_back(nodeName);
                ++written;
            }

            out << "\n  },\n";
            out << "  \"version\": 2\n";
            out << "}\n";

            if (written <= 0) {
                WriteBuildReport(failurePrefix + ": zero transforms written", targetRef, written, missingSource, 0, computeFailed);
                return false;
            }

            return true;
        }
        catch (...) {
            WriteBuildReport(failurePrefix + ": exception while writing output file", targetRef, 0, 0, 0, 0);
            return false;
        }
    }

    bool IsPoseClipboardLimbMirrorNode(const std::string& nodeName)
    {
        return nodeName.rfind("LArm_", 0) == 0 || nodeName.rfind("RArm_", 0) == 0 || nodeName.rfind("LLeg_", 0) == 0 || nodeName.rfind("RLeg_", 0) == 0;
    }

    bool WriteLimbOnlyMirroredPoseClipboardFile(const std::string& text, const std::filesystem::path& outputPath, const std::string& filename, TESObjectREFR* targetRef, const std::string& failurePrefix)
    {
        if (text.empty()) {
            WriteBuildReport(failurePrefix + ": clipboard file missing or empty", targetRef, 0, 0, 0, 0);
            return false;
        }

        const std::vector<std::string> sourceNodeNames = ExtractSafPoseTransformKeys(text);
        if (sourceNodeNames.empty()) {
            WriteBuildReport(failurePrefix + ": clipboard contains no transform entries", targetRef, 0, 0, 0, 0);
            return false;
        }

        try {
            std::vector<std::string> labels;
            ExtractPoseClipboardLabels(text, labels);

            std::filesystem::path tempPath = outputPath;
            tempPath += ".tmp";

            std::ofstream out(tempPath, std::ios::trunc);
            if (!out) {
                WriteBuildReport(failurePrefix + ": could not open temporary output file", targetRef, 0, 0, 0, 0);
                return false;
            }

            out << "{\n";
            out << "  \"name\": \"" << JsonEscape(filename) << "\",\n";
            out << "  \"skeleton\": \"All\",\n";
            if (!labels.empty()) {
                out << "  \"clipboardLabels\": [";
                for (std::size_t i = 0; i < labels.size(); ++i) {
                    if (i > 0) {
                        out << ", ";
                    }
                    out << "\"" << JsonEscape(labels[i]) << "\"";
                }
                out << "],\n";
            }
            out << "  \"transforms\": {\n";

            int written = 0;
            int missingSource = 0;
            int computeFailed = 0;
            std::vector<std::string> writtenNodes;

            for (const auto& sourceName : sourceNodeNames) {
                if (sourceName.empty()) {
                    ++computeFailed;
                    continue;
                }
                const std::string destinationName = GetSidewaysMirroredNodeName(sourceName);
                if (!IsPoseClipboardLimbMirrorNode(sourceName) || destinationName == sourceName) {
                    continue;
                }
                WriteFullMirroredSavedPoseNode(out, text, sourceName, destinationName, true, writtenNodes, missingSource, computeFailed, written);
            }

            for (const auto& sourceName : sourceNodeNames) {
                if (sourceName.empty() || WasNodeAlreadyWritten(writtenNodes, sourceName)) {
                    continue;
                }
                const std::string destinationName = GetSidewaysMirroredNodeName(sourceName);
                if (IsPoseClipboardLimbMirrorNode(sourceName) && destinationName != sourceName) {
                    continue;
                }

                PoseNode data;
                if (!ExtractSafPoseTransform(text, sourceName, data)) {
                    ++missingSource;
                    continue;
                }

                if (written > 0) {
                    out << ",\n";
                }
                WriteSafPoseTransformJson(out, sourceName, data.posX, data.posY, data.posZ, data.yaw, data.pitch, data.roll, data.scale);
                writtenNodes.push_back(sourceName);
                ++written;
            }

            out << "\n  },\n";
            out << "  \"version\": 2\n";
            out << "}\n";
            out.close();

            if (written <= 0) {
                std::error_code removeEc;
                std::filesystem::remove(tempPath, removeEc);
                WriteBuildReport(failurePrefix + ": zero mirrored transforms written", targetRef, written, missingSource, 0, computeFailed);
                return false;
            }

            std::error_code ec;
            std::filesystem::rename(tempPath, outputPath, ec);
            if (ec) {
                ec.clear();
                std::filesystem::remove(outputPath, ec);
                ec.clear();
                std::filesystem::rename(tempPath, outputPath, ec);
                if (ec) {
                    WriteBuildReport(failurePrefix + ": could not replace clipboard file", targetRef, written, missingSource, 0, computeFailed);
                    return false;
                }
            }

            return true;
        }
        catch (...) {
            WriteBuildReport(failurePrefix + ": exception while writing mirrored clipboard", targetRef, 0, 0, 0, 0);
            return false;
        }
    }

    bool MirrorPoseClipboardInternal()
    {
        {
            std::error_code ec;
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
        }
        const auto path = GetPoseClipboardPath();
        const std::string text = ReadTextFile(path);
        return WriteLimbOnlyMirroredPoseClipboardFile(text, path, "Straw_PoseClipboard_PlayIdle", nullptr, "pose clipboard limb mirror failed");
    }

    bool MirrorPoseClipboardSamInternal()
    {
        {
            std::error_code ec;
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
        }
        const auto path = GetPoseClipboardSamPath();
        const std::string text = ReadTextFile(path);
        return WriteLimbOnlyMirroredPoseClipboardFile(text, path, "Straw_PoseClipboard_SamPose", nullptr, "pose clipboard SAM limb mirror failed");
    }

    bool BuildPoseClipboardFromSavedPoseFileInternal(TESObjectREFR* targetRef, const std::vector<std::string>& sourceNodes, const std::string& label)
    {
        {
            std::error_code ec;
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
        }

        if (!targetRef || sourceNodes.empty()) {
            WriteBuildReport("pose clipboard copy failed: invalid target or bone list", targetRef, 0, 0, 0, 0);
            return false;
        }

        std::filesystem::path inputPath;
        std::vector<std::filesystem::path> triedInputPaths;
        const std::string text = ReadPoseClipboardSavedPoseText(inputPath, triedInputPaths);
        if (text.empty()) {
            WriteBuildReport("pose clipboard copy failed: SavePose probe file missing or empty in all candidate paths", targetRef, 0, 0, 0, 0);
            return false;
        }

        const std::string existingText = ReadTextFile(GetPoseClipboardPath());
        return WritePoseClipboardMergedFile(existingText, text, sourceNodes, "Straw_PoseClipboard_PlayIdle", GetPoseClipboardPath(), label, targetRef, "pose clipboard copy failed", false);
    }

    bool BuildPoseClipboardFromSavedAdjustmentFileInternal(TESObjectREFR* targetRef, const std::vector<std::string>& sourceNodes, const std::string& label)
    {
        {
            std::error_code ec;
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
        }

        if (!targetRef || sourceNodes.empty()) {
            WriteBuildReport("pose clipboard SAM copy failed: invalid target or bone list", targetRef, 0, 0, 0, 0);
            return false;
        }

        std::filesystem::path inputPath;
        std::vector<std::filesystem::path> triedInputPaths;
        const std::string text = ReadPoseClipboardSavedAdjustmentText(inputPath, triedInputPaths);
        if (text.empty()) {
            WriteBuildReport("pose clipboard SAM copy failed: saved adjustment file missing or empty in all candidate paths", targetRef, 0, 0, 0, 0);
            return false;
        }

        const std::string existingText = ReadTextFile(GetPoseClipboardSamPath());
        return WritePoseClipboardMergedFile(existingText, text, sourceNodes, "Straw_PoseClipboard_SamPose", GetPoseClipboardSamPath(), label, targetRef, "pose clipboard SAM copy failed", true);
    }

    bool BuildPoseClipboardPastePoseFileInternal(TESObjectREFR* targetRef)
    {
        {
            std::error_code ec;
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
        }

        if (!targetRef) {
            WriteBuildReport("pose clipboard paste failed: invalid target", targetRef, 0, 0, 0, 0);
            return false;
        }

        const std::string text = ReadTextFile(GetPoseClipboardPath());
        if (text.empty()) {
            WriteBuildReport("pose clipboard paste failed: clipboard file missing or empty", targetRef, 0, 0, 0, 0);
            return false;
        }

        const std::vector<std::string> nodes = ExtractSafPoseTransformKeys(text);
        if (nodes.empty()) {
            WriteBuildReport("pose clipboard paste failed: clipboard contains no transforms", targetRef, 0, 0, 0, 0);
            return false;
        }

        return WritePoseClipboardPoseFile(text, nodes, "Straw_PoseClipboard_PastePose", GetPoseClipboardPastePosePath(), "", targetRef, "pose clipboard paste failed");
    }

    bool BuildPoseClipboardPasteAdjustmentFileInternal(TESObjectREFR* targetRef)
    {
        {
            std::error_code ec;
            std::filesystem::remove(GetPoseMirrorPoseBuildErrorPath(), ec);
        }

        if (!targetRef) {
            WriteBuildReport("pose clipboard SAM paste failed: invalid target", targetRef, 0, 0, 0, 0);
            return false;
        }

        const std::string text = ReadTextFile(GetPoseClipboardSamPath());
        if (text.empty()) {
            WriteBuildReport("pose clipboard SAM paste failed: clipboard file missing or empty", targetRef, 0, 0, 0, 0);
            return false;
        }

        const std::vector<std::string> nodes = ExtractSafPoseTransformKeys(text);
        if (nodes.empty()) {
            WriteBuildReport("pose clipboard SAM paste failed: clipboard contains no transforms", targetRef, 0, 0, 0, 0);
            return false;
        }

        std::filesystem::path destinationPath;
        std::vector<std::filesystem::path> triedDestinationPaths;
        const std::string destinationText = ReadPoseClipboardDestinationAdjustmentText(destinationPath, triedDestinationPaths);

        try {
            const auto outputPath = GetPoseClipboardActiveAdjustmentPath();
            std::ofstream out(outputPath, std::ios::trunc);
            if (!out) {
                WriteBuildReport("pose clipboard SAM paste failed: could not open output file", targetRef, 0, 0, 0, 0);
                return false;
            }

            out << "{\n";
            out << "  \"name\": \"CopyPastePoseLimbs Data\",\n";
            out << "  \"skeleton\": \"All\",\n";
            out << "  \"transforms\": {\n";

            int written = 0;
            int missingSource = 0;
            int missingDestination = 0;
            int computeFailed = 0;
            std::vector<std::string> writtenNodes;

            for (const auto& nodeName : nodes) {
                if (nodeName.empty() || WasNodeAlreadyWritten(writtenNodes, nodeName)) {
                    ++computeFailed;
                    continue;
                }

                PoseNode source;
                if (!ExtractSafPoseTransform(text, nodeName, source)) {
                    ++missingSource;
                    continue;
                }

                PoseNode destination;
                if (!destinationText.empty() && ExtractSafPoseTransform(destinationText, nodeName, destination)) {
                    PoseNode overlay = MultiplyPoseNode(InvertPoseNode(destination), source);
                    source = overlay;
                }
                else {
                    ++missingDestination;
                }

                if (written > 0) {
                    out << ",\n";
                }
                WriteSafPoseTransformJson(out, nodeName, source.posX, source.posY, source.posZ, source.yaw, source.pitch, source.roll, source.scale);
                writtenNodes.push_back(nodeName);
                ++written;
            }

            out << "\n  },\n";
            out << "  \"version\": 2\n";
            out << "}\n";

            if (written <= 0) {
                WriteBuildReport("pose clipboard SAM paste failed: zero transforms written", targetRef, written, missingSource, missingDestination, computeFailed);
                return false;
            }

            return true;
        }
        catch (...) {
            WriteBuildReport("pose clipboard SAM paste failed: exception while writing output file", targetRef, 0, 0, 0, 0);
            return false;
        }
    }

    void WriteBuildReport(const std::string& reason, TESObjectREFR* targetRef, int written, int missingSource, int missingDestination, int computeFailed)
    {
        try {
            const auto path = GetPoseMirrorPoseBuildErrorPath();
            std::ofstream out(path, std::ios::trunc);
            if (!out) {
                return;
            }
            out << "{\n";
            out << "  \"probeVersion\": \"Straw_SamExtensions v1.7.50 / Straw_SamExtensions 0.1.75\",\n";
            out << "  \"reason\": \"" << JsonEscape(reason) << "\",\n";
            out << "  \"targetFormId\": \"" << (targetRef ? FormatFormId(targetRef->formID) : std::string("00000000")) << "\",\n";
            out << "  \"dllPluginDirectory\": \"" << JsonEscape(GetPluginDirectory().string()) << "\",\n";
            out << "  \"runtimePluginDirectory\": \"" << JsonEscape(GetRuntimePluginDirectory().string()) << "\",\n";
            out << "  \"savePoseProbePath\": \"" << JsonEscape(GetPoseMirrorSavedPoseProbePath().string()) << "\",\n";
            out << "  \"writtenTransforms\": " << written << ",\n";
            out << "  \"missingSourceNodes\": " << missingSource << ",\n";
            out << "  \"missingDestinationNodes\": " << missingDestination << ",\n";
            out << "  \"computeFailedNodes\": " << computeFailed << "\n";
            out << "}\n";
        }
        catch (...) {
        }
    }


    bool PapyrusCleanupPoseMirrorTemporaryFiles(StaticFunctionTag*)
    {
        return CleanupPoseMirrorTemporaryFilesInternal();
    }

    bool PapyrusDeletePoseMirrorAdjustmentFile(StaticFunctionTag*, BSFixedString fileName)
    {
        const char* rawFileName = fileName.c_str();
        if (!rawFileName) {
            return false;
        }
        return DeletePoseMirrorAdjustmentFileInternal(rawFileName);
    }


    bool PapyrusBuildPoseMirrorFromSavedPoseFile(StaticFunctionTag*, TESObjectREFR* targetRef, BSFixedString sourceNodesPipe, BSFixedString destinationNodesPipe, SInt32 mode)
    {
        const char* rawSource = sourceNodesPipe.c_str();
        const char* rawDestination = destinationNodesPipe.c_str();
        if (!targetRef || !rawSource || !rawDestination) {
            return false;
        }

        const std::vector<std::string> sourceNodes = SplitPipeList(rawSource);
        const std::vector<std::string> destinationNodes = SplitPipeList(rawDestination);
        return BuildPoseMirrorFromSavedPoseFileInternal(targetRef, sourceNodes, destinationNodes, "Straw_PoseMirror_PlayIdle_MirroredPose", mode);
    }


    bool PapyrusBuildFullPoseMirrorFromSavedPoseFile(StaticFunctionTag*, TESObjectREFR* targetRef)
    {
        if (!targetRef) {
            return false;
        }
        return BuildFullPoseMirrorFromSavedPoseFileInternal(targetRef, "Straw_PoseMirror_PlayIdle_MirroredPose");
    }



    bool PapyrusBuildPoseClipboardFromSavedPoseFile(StaticFunctionTag*, TESObjectREFR* targetRef, BSFixedString sourceNodesPipe, BSFixedString label)
    {
        const char* rawSource = sourceNodesPipe.c_str();
        const char* rawLabel = label.c_str();
        if (!targetRef || !rawSource) {
            return false;
        }

        const std::vector<std::string> sourceNodes = ExpandPoseClipboardNodeList(SplitPipeList(rawSource));
        return BuildPoseClipboardFromSavedPoseFileInternal(targetRef, sourceNodes, rawLabel ? rawLabel : "");
    }

    bool PapyrusBuildPoseClipboardPastePoseFile(StaticFunctionTag*, TESObjectREFR* targetRef)
    {
        if (!targetRef) {
            return false;
        }
        return BuildPoseClipboardPastePoseFileInternal(targetRef);
    }

    bool PapyrusBuildPoseClipboardFromSavedAdjustmentFile(StaticFunctionTag*, TESObjectREFR* targetRef, BSFixedString sourceNodesPipe, BSFixedString label)
    {
        const char* rawSource = sourceNodesPipe.c_str();
        const char* rawLabel = label.c_str();
        if (!targetRef || !rawSource) {
            return false;
        }

        const std::vector<std::string> sourceNodes = ExpandPoseClipboardNodeList(SplitPipeList(rawSource));
        return BuildPoseClipboardFromSavedAdjustmentFileInternal(targetRef, sourceNodes, rawLabel ? rawLabel : "");
    }

    bool PapyrusBuildPoseClipboardPasteAdjustmentFile(StaticFunctionTag*, TESObjectREFR* targetRef)
    {
        if (!targetRef) {
            return false;
        }
        return BuildPoseClipboardPasteAdjustmentFileInternal(targetRef);
    }

    bool PapyrusMirrorPoseClipboard(StaticFunctionTag*)
    {
        return MirrorPoseClipboardInternal();
    }

    bool PapyrusMirrorPoseClipboardSam(StaticFunctionTag*)
    {
        return MirrorPoseClipboardSamInternal();
    }

    bool PapyrusCleanupPoseClipboardTemporaryFiles(StaticFunctionTag*)
    {
        return CleanupPoseClipboardTemporaryFilesInternal();
    }

    bool PapyrusDeletePoseClipboardFiles(StaticFunctionTag*)
    {
        return DeletePoseClipboardFilesInternal();
    }

    bool PapyrusDeletePoseClipboardSamFiles(StaticFunctionTag*)
    {
        return DeletePoseClipboardSamFilesInternal();
    }

    bool PapyrusHasPoseClipboard(StaticFunctionTag*)
    {
        return HasPoseClipboardInternal();
    }

    bool PapyrusHasPoseClipboardSam(StaticFunctionTag*)
    {
        return HasPoseClipboardSamInternal();
    }

    SInt32 PapyrusGetPoseClipboardLimbCount(StaticFunctionTag*)
    {
        return static_cast<SInt32>(GetPoseClipboardLimbCountInternal());
    }

    SInt32 PapyrusGetPoseClipboardSamLimbCount(StaticFunctionTag*)
    {
        return static_cast<SInt32>(GetPoseClipboardSamLimbCountInternal());
    }

    VMArray<BSFixedString> PapyrusGetRootTxtFiles(StaticFunctionTag*)
    {
        VMArray<BSFixedString> result;
        const auto root = GetGameRootDirectory();
        Log("Straw_SamExtensions: scanning root folder: %s", root.string().c_str());

        const auto names = ScanRootTxtFileStems();
        for (const auto& name : names) {
            BSFixedString fixed(name.c_str());
            result.Push(&fixed);
            fixed.Release();
        }

        Log("Straw_SamExtensions: found %zu root TXT file(s)", names.size());
        return result;
    }


    bool RunBatchFileInternal(const std::string& stem, UInt32 targetFormId)
    {
        if (!IsSafeBatchStem(stem)) {
            Log("Straw_SamExtensions: RunBatchFile rejected unsafe file name: %s", stem.c_str());
            return false;
        }

        if (!IsKnownRootTxtStem(stem)) {
            Log("Straw_SamExtensions: RunBatchFile rejected unknown root TXT stem: %s", stem.c_str());
            return false;
        }

        const auto path = GetGameRootDirectory() / (stem + ".txt");
        std::ifstream in(path);
        if (!in) {
            Log("Straw_SamExtensions: RunBatchFile failed to open: %s", path.string().c_str());
            return false;
        }

        Log("Straw_SamExtensions: RunBatchFile started: %s target=%08X", path.string().c_str(), targetFormId);

        std::string line;
        std::size_t lineNumber = 0;
        std::size_t executed = 0;
        while (std::getline(in, line)) {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            std::string command = TrimAscii(line);
            if (ShouldSkipBatchLine(command)) {
                continue;
            }

            if (command.size() > kMaxCommandLength) {
                Log("Straw_SamExtensions: RunBatchFile failed: line %zu too long", lineNumber);
                return false;
            }

            if (++executed > kMaxBatchCommands) {
                Log("Straw_SamExtensions: RunBatchFile failed: command limit exceeded (%zu)", kMaxBatchCommands);
                return false;
            }

            const auto actualCommand = ApplySelectedRefPrefixIfNeeded(command, targetFormId);
            if (!ExecuteScriptLine(actualCommand)) {
                Log("Straw_SamExtensions: RunBatchFile failed at line %zu: %s", lineNumber, actualCommand.c_str());
                return false;
            }
        }

        Log("Straw_SamExtensions: RunBatchFile finished: %s (%zu command(s))", stem.c_str(), executed);
        return true;
    }


    bool PapyrusRunBatchFileForTarget(StaticFunctionTag*, BSFixedString fileName, SInt32 targetFormId)
    {
        const char* raw = fileName.c_str();
        if (!raw || !raw[0]) {
            Log("Straw_SamExtensions: RunBatchFileForTarget rejected empty file name");
            return false;
        }

        return RunBatchFileInternal(TrimAscii(raw), static_cast<UInt32>(targetFormId));
    }


    bool PapyrusStartVisualSpin(StaticFunctionTag*, TESObjectREFR* targetRef, float speedDeg, float direction)
    {
        return StartNativeSpinner(targetRef, nullptr, 1, speedDeg, direction);
    }

    bool PapyrusStartVisualFacePlayer(StaticFunctionTag*, TESObjectREFR* targetRef, TESObjectREFR* playerRef, float speedDeg)
    {
        return StartNativeSpinner(targetRef, playerRef, 2, speedDeg, 1.0f);
    }

    bool PapyrusStopVisualSpin(StaticFunctionTag*, TESObjectREFR* targetRef)
    {
        if (!targetRef) {
            StopAllNativeSpinners(true);
            return true;
        }

        std::shared_ptr<SpinState> st;
        {
            std::lock_guard<std::mutex> guard(g_spinLock);
            const auto it = g_spins.find(targetRef->formID);
            if (it != g_spins.end()) {
                st = it->second;
                g_spins.erase(it);
                g_spinWake.notify_all();
            }
        }

        if (st) {
            st->active.store(false);
            QueueRestoreTask(st);
        }
        return true;
    }

    bool PapyrusPauseVisualSpin(StaticFunctionTag*, TESObjectREFR* targetRef)
    {
        if (!targetRef) {
            PauseAllNativeSpinners();
            return true;
        }

        std::shared_ptr<SpinState> st;
        {
            std::lock_guard<std::mutex> guard(g_spinLock);
            const auto it = g_spins.find(targetRef->formID);
            if (it != g_spins.end()) {
                st = it->second;
                if (st) {
                    st->paused.store(true);
                }
                g_spinWake.notify_all();
            }
        }

        if (st) {
            QueuePauseTask(st);
        }
        return true;
    }

    SInt32 PapyrusGetVisualSpinCount(StaticFunctionTag*)
    {
        return GetActiveSpinnerCount();
    }

    SInt32 PapyrusGetMaxActiveSpinners(StaticFunctionTag*)
    {
        EnsureSpinnerConfigLoaded();
        return g_maxActiveSpinners;
    }

    bool PapyrusIsVisualSpinActive(StaticFunctionTag*, TESObjectREFR* targetRef)
    {
        return IsNativeSpinnerActive(targetRef);
    }

    bool RegisterPapyrus(VirtualMachine* vm)
    {
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, VMArray<BSFixedString>>("GetRootTxtFiles", "Straw_SamExtensions", PapyrusGetRootTxtFiles, vm));
        vm->RegisterFunction(new NativeFunction4<StaticFunctionTag, bool, TESObjectREFR*, BSFixedString, BSFixedString, SInt32>("BuildPoseMirrorFromSavedPoseFile", "Straw_SamExtensions", PapyrusBuildPoseMirrorFromSavedPoseFile, vm));
        vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, TESObjectREFR*>("BuildFullPoseMirrorFromSavedPoseFile", "Straw_SamExtensions", PapyrusBuildFullPoseMirrorFromSavedPoseFile, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>("CleanupPoseMirrorTemporaryFiles", "Straw_SamExtensions", PapyrusCleanupPoseMirrorTemporaryFiles, vm));
        vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, BSFixedString>("DeletePoseMirrorAdjustmentFile", "Straw_SamExtensions", PapyrusDeletePoseMirrorAdjustmentFile, vm));
        vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, bool, TESObjectREFR*, BSFixedString, BSFixedString>("BuildPoseClipboardFromSavedPoseFile", "Straw_SamExtensions", PapyrusBuildPoseClipboardFromSavedPoseFile, vm));
        vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, TESObjectREFR*>("BuildPoseClipboardPastePoseFile", "Straw_SamExtensions", PapyrusBuildPoseClipboardPastePoseFile, vm));
        vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, bool, TESObjectREFR*, BSFixedString, BSFixedString>("BuildPoseClipboardFromSavedAdjustmentFile", "Straw_SamExtensions", PapyrusBuildPoseClipboardFromSavedAdjustmentFile, vm));
        vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, TESObjectREFR*>("BuildPoseClipboardPasteAdjustmentFile", "Straw_SamExtensions", PapyrusBuildPoseClipboardPasteAdjustmentFile, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>("MirrorPoseClipboard", "Straw_SamExtensions", PapyrusMirrorPoseClipboard, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>("MirrorPoseClipboardSam", "Straw_SamExtensions", PapyrusMirrorPoseClipboardSam, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>("CleanupPoseClipboardTemporaryFiles", "Straw_SamExtensions", PapyrusCleanupPoseClipboardTemporaryFiles, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>("DeletePoseClipboardFiles", "Straw_SamExtensions", PapyrusDeletePoseClipboardFiles, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>("DeletePoseClipboardSamFiles", "Straw_SamExtensions", PapyrusDeletePoseClipboardSamFiles, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>("HasPoseClipboard", "Straw_SamExtensions", PapyrusHasPoseClipboard, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, bool>("HasPoseClipboardSam", "Straw_SamExtensions", PapyrusHasPoseClipboardSam, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, SInt32>("GetPoseClipboardLimbCount", "Straw_SamExtensions", PapyrusGetPoseClipboardLimbCount, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, SInt32>("GetPoseClipboardSamLimbCount", "Straw_SamExtensions", PapyrusGetPoseClipboardSamLimbCount, vm));
        vm->RegisterFunction(new NativeFunction2<StaticFunctionTag, bool, BSFixedString, SInt32>("RunBatchFileForTarget", "Straw_SamExtensions", PapyrusRunBatchFileForTarget, vm));
        vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, bool, TESObjectREFR*, float, float>("StartVisualSpin", "Straw_SamExtensions", PapyrusStartVisualSpin, vm));
        vm->RegisterFunction(new NativeFunction3<StaticFunctionTag, bool, TESObjectREFR*, TESObjectREFR*, float>("StartVisualFacePlayer", "Straw_SamExtensions", PapyrusStartVisualFacePlayer, vm));
        vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, TESObjectREFR*>("StopVisualSpin", "Straw_SamExtensions", PapyrusStopVisualSpin, vm));
        vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, TESObjectREFR*>("PauseVisualSpin", "Straw_SamExtensions", PapyrusPauseVisualSpin, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, SInt32>("GetVisualSpinCount", "Straw_SamExtensions", PapyrusGetVisualSpinCount, vm));
        vm->RegisterFunction(new NativeFunction0<StaticFunctionTag, SInt32>("GetMaxActiveSpinners", "Straw_SamExtensions", PapyrusGetMaxActiveSpinners, vm));
        vm->RegisterFunction(new NativeFunction1<StaticFunctionTag, bool, TESObjectREFR*>("IsVisualSpinActive", "Straw_SamExtensions", PapyrusIsVisualSpinActive, vm));
        return true;
    }
}

extern "C"
{
    __declspec(dllexport) bool F4SEPlugin_Query(const F4SEInterface* f4se, PluginInfo* info)
    {
        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = "Straw_SamExtensions";
        info->version = kPluginVersion;

        if (f4se->isEditor) {
            Log("Straw_SamExtensions: loaded in editor, refusing to load");
            return false;
        }

        Log("Straw_SamExtensions: query OK. F4SE=%08X runtime=%08X", f4se->f4seVersion, f4se->runtimeVersion);
        return true;
    }

    __declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se)
    {
        g_pluginHandle = f4se->GetPluginHandle();

        g_task = static_cast<F4SETaskInterface*>(f4se->QueryInterface(kInterface_Task));
        if (!g_task) {
            Log("Straw_SamExtensions: failed to query task interface");
            return false;
        }

        auto* papyrus = static_cast<F4SEPapyrusInterface*>(f4se->QueryInterface(kInterface_Papyrus));
        if (!papyrus) {
            Log("Straw_SamExtensions: failed to query Papyrus interface");
            return false;
        }

        if (!papyrus->Register(RegisterPapyrus)) {
            Log("Straw_SamExtensions: failed to register Papyrus functions");
            return false;
        }

        Log("Straw_SamExtensions: loaded and registered Papyrus functions");
        return true;
    }
}
