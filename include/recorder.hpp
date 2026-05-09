#pragma once

#include "GlobalNamespace/NoteController.hpp"
#include "GlobalNamespace/NoteCutInfo.hpp"
#include "GlobalNamespace/PracticeSettings.hpp"
#include "GlobalNamespace/Saber.hpp"
#include "replay.hpp"

namespace Recorder {
    // Called when a practice mode level starts
    void OnPracticeStart(GlobalNamespace::PracticeSettings* practiceSettings, std::string mapHash);

    // Called every frame to capture head/saber poses
    void OnUpdate(
        UnityEngine::Transform* head,
        GlobalNamespace::Saber* leftSaber,
        GlobalNamespace::Saber* rightSaber,
        float songTime,
        int fps
    );

    // Called on a good or bad note cut
    void OnNoteCut(GlobalNamespace::NoteController* note, GlobalNamespace::NoteCutInfo const& cutInfo);

    // Called when a note is missed
    void OnNoteMiss(GlobalNamespace::NoteController* note);

    // Called when a bomb is cut
    void OnBombCut(GlobalNamespace::NoteController* bomb, GlobalNamespace::NoteCutInfo const& cutInfo);

    // Called when the level ends (completed, failed, or quit)
    void OnLevelEnd(bool quit, bool failed, float failTime);

    // Called when the level is paused/unpaused
    void OnPause(float songTime);
    void OnUnpause(float songTime);

    // Returns true if currently recording a practice session
    bool IsRecording();

    // Saves the captured replay to disk as a .bsor file
    // Returns the file path on success, empty string on failure
    std::string SaveReplay();
}
