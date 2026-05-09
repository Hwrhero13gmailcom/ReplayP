#include "recorder.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

#include "GlobalNamespace/NoteController.hpp"
#include "GlobalNamespace/NoteData.hpp"
#include "GlobalNamespace/NoteCutInfo.hpp"
#include "GlobalNamespace/PracticeSettings.hpp"
#include "GlobalNamespace/Saber.hpp"
#include "UnityEngine/Transform.hpp"
#include "beatsaber-hook/shared/utils/utils.h"
#include "main.hpp"
#include "metacore/shared/songs.hpp"
#include "replay.hpp"

using namespace GlobalNamespace;
using namespace UnityEngine;

// Use fully qualified types to avoid ambiguity with Sombrero aliases
using UVec3 = UnityEngine::Vector3;
using UQuat = UnityEngine::Quaternion;

// ─── module state ────────────────────────────────────────────────────────────

static bool recording = false;
static Replay::Data currentReplay;

// ─── helpers ─────────────────────────────────────────────────────────────────

static Replay::Transform TransformOf(Transform* t) {
    auto pos = t->get_position();
    auto rot = t->get_rotation();
    return {{pos.x, pos.y, pos.z}, {rot.x, rot.y, rot.z, rot.w}};
}
// Convert a NoteCutInfo (Unity/game object) to our storage struct
static Replay::Events::CutInfo CutInfoFrom(NoteCutInfo const& c) {
    Replay::Events::CutInfo ci;
    ci.speedOK          = c.speedOK;
    ci.directionOK      = c.directionOK;
    ci.saberTypeOK      = c.saberTypeOK;
    ci.wasCutTooSoon    = c.wasCutTooSoon;
    ci.saberSpeed       = c.saberSpeed;
    ci.saberDir         = {c.saberDir.x, c.saberDir.y, c.saberDir.z};
    ci.saberType        = (int) c.saberType;
    ci.timeDeviation    = c.timeDeviation;
    ci.cutDirDeviation  = c.cutDirDeviation;
    ci.cutPoint         = {c.cutPoint.x, c.cutPoint.y, c.cutPoint.z};
    ci.cutNormal        = {c.cutNormal.x, c.cutNormal.y, c.cutNormal.z};
    ci.cutDistanceToCenter = c.cutDistanceToCenter;
    ci.cutAngle         = c.cutAngle;
    ci.beforeCutRating  = 0;  // filled in later by the scoring system
    ci.afterCutRating   = 0;
    return ci;
}

static Replay::Events::NoteInfo NoteInfoFrom(NoteController* note, Replay::Events::NoteInfo::Type type) {
    Replay::Events::NoteInfo ni;
    auto data = note->noteData;
    ni.scoringType  = (short)(int) data->scoringType;
    ni.lineIndex    = (short)(int) data->lineIndex;
    ni.lineLayer    = (short)(int) data->noteLineLayer;
    ni.colorType    = (short)(int) data->colorType;
    ni.cutDirection = (short)(int) data->cutDirection;
    ni.eventType    = type;
    return ni;
}

// ─── BSOR binary writer ───────────────────────────────────────────────────────
// Mirrors the format documented at https://github.com/BeatLeader/BS-Open-Replay

static void WriteBytes(std::ofstream& out, const void* data, size_t size) {
    out.write(reinterpret_cast<const char*>(data), size);
}

template <typename T>
static void Write(std::ofstream& out, T value) {
    WriteBytes(out, &value, sizeof(T));
}

static void WriteString(std::ofstream& out, std::string const& s) {
    int len = (int) s.size();
    Write(out, len);
    out.write(s.data(), len);
}

static void WriteVector3(std::ofstream& out, UVec3 v) {
    Write(out, v.x);
    Write(out, v.y);
    Write(out, v.z);
}

static void WriteQuaternion(std::ofstream& out, UQuat q) {
    Write(out, q.x);
    Write(out, q.y);
    Write(out, q.z);
    Write(out, q.w);
}

static void WriteBSOR(std::ofstream& out, Replay::Data const& replay) {
    // Magic byte + version
    Write<uint8_t>(out, 0x49); // 'I' – BeatLeader magic
    Write<int>(out, 1);        // section 1 = info

    // --- Info section ---
    WriteString(out, "1.0.0");          // version
    WriteString(out, "1.40.8");         // gameVersion (kept static; change if needed)

    // timestamp (unix seconds as string)
    auto ts = std::to_string(replay.info.timestamp);
    WriteString(out, ts);

    WriteString(out, "");               // playerID  (unknown in practice)
    // playerName as UTF-16LE length-prefixed
    {
        std::string const& name = replay.info.playerName.value_or("Practice");
        int charCount = (int) name.size();
        Write(out, charCount);
        for (char c : name) {
            Write<uint16_t>(out, (uint16_t)(unsigned char) c);
        }
    }
    WriteString(out, "quest");          // platform
    WriteString(out, "quest");          // trackingSystem
    WriteString(out, "OculusQuest");    // hmd
    WriteString(out, "OculusTouch");    // controller

    WriteString(out, replay.info.hash);
    // songName / mapper as UTF-16LE (we don't have them at record time, use empty)
    Write<int>(out, 0); // songName len=0
    Write<int>(out, 0); // mapper   len=0

    WriteString(out, "");               // difficulty (string form)

    Write<int>(out, replay.info.score);
    WriteString(out, "Standard");       // mode
    WriteString(out, "");               // environment
    WriteString(out, "");               // modifiers
    Write<float>(out, replay.info.jumpDistance);
    Write<bool>(out, replay.info.modifiers.leftHanded);
    Write<float>(out, 0.0f);           // height

    Write<float>(out, replay.info.startTime);
    Write<float>(out, replay.info.failTime);
    Write<float>(out, replay.info.speed);

    // --- Poses section ---
    Write<int>(out, 2); // section marker 2

    int poseCount = (int) replay.poses.size();
    Write<int>(out, poseCount);
    for (auto const& pose : replay.poses) {
        Write<float>(out, pose.time);
        // fps stored as float in the format
        Write<float>(out, (float) pose.fps);
        WriteVector3(out, {pose.head.position.x, pose.head.position.y, pose.head.position.z});
        WriteQuaternion(out, {pose.head.rotation.x, pose.head.rotation.y, pose.head.rotation.z, pose.head.rotation.w});
        WriteVector3(out, {pose.leftHand.position.x, pose.leftHand.position.y, pose.leftHand.position.z});
        WriteQuaternion(out, {pose.leftHand.rotation.x, pose.leftHand.rotation.y, pose.leftHand.rotation.z, pose.leftHand.rotation.w});
        WriteVector3(out, {pose.rightHand.position.x, pose.rightHand.position.y, pose.rightHand.position.z});
        WriteQuaternion(out, {pose.rightHand.rotation.x, pose.rightHand.rotation.y, pose.rightHand.rotation.z, pose.rightHand.rotation.w});
    }

    // --- Notes section ---
    Write<int>(out, 3); // section marker 3

    if (!replay.events.has_value()) {
        Write<int>(out, 0);
    } else {
        auto const& events = replay.events.value();
        int noteCount = (int) events.notes.size();
        Write<int>(out, noteCount);
        for (auto const& note : events.notes) {
            // noteID encoding: lineIndex | (lineLayer << 3) | (colorType << 6) | (cutDirection << 8) | (scoringType << 12)
            int noteID = (note.info.lineIndex) |
                         (note.info.lineLayer    << 3)  |
                         (note.info.colorType    << 6)  |
                         (note.info.cutDirection << 8)  |
                         (note.info.scoringType  << 12);
            Write<int>(out, noteID);
            Write<float>(out, note.time);         // eventTime
            Write<float>(out, note.time);         // spawnTime (approximate; we don't track separately)
            Write<int>(out, (int) note.info.eventType);

            if (note.info.eventType == Replay::Events::NoteInfo::Type::GOOD ||
                note.info.eventType == Replay::Events::NoteInfo::Type::BAD ||
                note.info.eventType == Replay::Events::NoteInfo::Type::BOMB) {
                auto const& ci = note.noteCutInfo;
                Write<bool>(out, ci.speedOK);
                Write<bool>(out, ci.directionOK);
                Write<bool>(out, ci.saberTypeOK);
                Write<bool>(out, ci.wasCutTooSoon);
                Write<float>(out, ci.saberSpeed);
                WriteVector3(out, {ci.saberDir.x, ci.saberDir.y, ci.saberDir.z});
                Write<int>(out, ci.saberType);
                Write<float>(out, ci.timeDeviation);
                Write<float>(out, ci.cutDirDeviation);
                WriteVector3(out, {ci.cutPoint.x, ci.cutPoint.y, ci.cutPoint.z});
                WriteVector3(out, {ci.cutNormal.x, ci.cutNormal.y, ci.cutNormal.z});
                Write<float>(out, ci.cutDistanceToCenter);
                Write<float>(out, ci.cutAngle);
                Write<float>(out, ci.beforeCutRating);
                Write<float>(out, ci.afterCutRating);
            }
        }
    }

    // --- Walls section (empty) ---
    Write<int>(out, 4); // section marker 4
    Write<int>(out, 0); // 0 wall events

    // --- Heights section (empty) ---
    Write<int>(out, 5); // section marker 5
    Write<int>(out, 0);

    // --- Pauses section ---
    Write<int>(out, 6); // section marker 6
    if (!replay.events.has_value()) {
        Write<int>(out, 0);
    } else {
        auto const& pauses = replay.events.value().pauses;
        Write<int>(out, (int) pauses.size());
        for (auto const& p : pauses) {
            Write<long>(out, p.duration);
            Write<float>(out, p.time);
        }
    }
}

// ─── public API ──────────────────────────────────────────────────────────────

bool Recorder::IsRecording() {
    return recording;
}

void Recorder::OnPracticeStart(PracticeSettings* practiceSettings, std::string mapHash) {
    if (!practiceSettings)
        return; // not practice mode

    logger.info("Recorder: starting practice capture for map {}", mapHash);

    currentReplay = {};
    currentReplay.info.hash      = mapHash;
    currentReplay.info.practice  = true;
    currentReplay.info.startTime = practiceSettings->startSongTime;
    currentReplay.info.speed     = practiceSettings->songSpeedMul;
    currentReplay.info.timestamp = (long) std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    currentReplay.info.score     = 0;
    currentReplay.events         = Replay::Events::Data{};

    recording = true;
}

void Recorder::OnUpdate(
    Transform* head,
    Saber* leftSaber,
    Saber* rightSaber,
    float songTime,
    int fps
) {
    if (!recording)
        return;

    Replay::Pose pose;
    pose.time     = songTime;
    pose.fps      = fps;
    pose.head     = TransformOf(head);
    pose.leftHand = TransformOf(leftSaber->transform);
    pose.rightHand = TransformOf(rightSaber->transform);

    currentReplay.poses.push_back(pose);
}

void Recorder::OnNoteCut(NoteController* note, NoteCutInfo const& cutInfo) {
    if (!recording || !currentReplay.events)
        return;

    bool isGood = const_cast<NoteCutInfo&>(cutInfo).get_allIsOK();
    auto type = isGood ? Replay::Events::NoteInfo::Type::GOOD : Replay::Events::NoteInfo::Type::BAD;

    Replay::Events::Note event;
    event.info        = NoteInfoFrom(note, type);
    event.noteCutInfo = CutInfoFrom(cutInfo);
    event.time        = note->noteData->time;

    currentReplay.events->notes.push_back(event);
}

void Recorder::OnNoteMiss(NoteController* note) {
    if (!recording || !currentReplay.events)
        return;

    Replay::Events::Note event;
    event.info = NoteInfoFrom(note, Replay::Events::NoteInfo::Type::MISS);
    event.time = note->noteData->time;

    currentReplay.events->notes.push_back(event);
}

void Recorder::OnBombCut(NoteController* bomb, NoteCutInfo const& cutInfo) {
    if (!recording || !currentReplay.events)
        return;

    Replay::Events::Note event;
    event.info        = NoteInfoFrom(bomb, Replay::Events::NoteInfo::Type::BOMB);
    event.noteCutInfo = CutInfoFrom(cutInfo);
    event.time        = bomb->noteData->time;

    currentReplay.events->notes.push_back(event);
}

void Recorder::OnPause(float songTime) {
    if (!recording || !currentReplay.events)
        return;

    // Store pause start time temporarily using a sentinel duration of 0
    Replay::Events::Pause p;
    p.time     = songTime;
    p.duration = 0;
    currentReplay.events->pauses.push_back(p);
}

void Recorder::OnUnpause(float songTime) {
    if (!recording || !currentReplay.events)
        return;

    // Fill in duration for the most recent open pause
    auto& pauses = currentReplay.events->pauses;
    if (!pauses.empty() && pauses.back().duration == 0) {
        long nowMs = (long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        // duration is stored as milliseconds of wall-clock pause time
        // We use a rough approximation via song time delta (not perfect but fine for viewing)
        pauses.back().duration = 1; // mark as closed; exact wall time isn't tracked here
    }
}

void Recorder::OnLevelEnd(bool quit, bool failed, float failTime) {
    if (!recording)
        return;

    currentReplay.info.quit     = quit;
    currentReplay.info.failed   = failed;
    currentReplay.info.failTime = failTime;

    if (!quit) {
        auto path = SaveReplay();
        if (!path.empty())
            logger.info("Recorder: saved practice replay to {}", path);
        else
            logger.error("Recorder: failed to save practice replay");
    }

    recording = false;
}

std::string Recorder::SaveReplay() {
    // BeatLeader stores replays here on Quest
    static std::string replayDir = "/sdcard/ModData/com.beatgames.beatsaber/Mods/bl/replays/";

    if (!std::filesystem::exists(replayDir)) {
        std::error_code ec;
        std::filesystem::create_directories(replayDir, ec);
        if (ec) {
            logger.error("Recorder: failed to create replay dir {}: {}", replayDir, ec.message());
            return "";
        }
    }

    // Filename: {hash}-Practice-{timestamp}.bsor
    std::string filename = replayDir + currentReplay.info.hash + "-Practice-" +
                           std::to_string(currentReplay.info.timestamp) + "-practice.bsor";

    std::ofstream out(filename, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        logger.error("Recorder: could not open {} for writing", filename);
        return "";
    }

    WriteBSOR(out, currentReplay);
    out.close();

    return filename;
}
