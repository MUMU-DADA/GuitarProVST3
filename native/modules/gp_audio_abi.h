#pragma once

// Minimal declarations for the hash-verified Guitar Pro 8.1.1.17 audio
// objects.  The implementation remains owned by GPCore/GPRSE; this header
// only describes the methods used to bind EffectsChain instances to score
// tracks on the control thread.

#include <memory>
#include <string>
#include <vector>

namespace gp::core {
class Sound;
class Track;
class ScoreModel;
class ScoreCursor {
public:
    int trackIndex() const;
};
class Score {
public:
    const std::vector<std::shared_ptr<Track>> &tracks() const;
    const std::shared_ptr<ScoreModel> &modelPrivate() const;
    ScoreCursor &cursor();
};
class Track {
public:
    const std::vector<std::shared_ptr<Sound>> &sounds() const;
};
}

namespace gp::rse {
class EffectsChain {
public:
    unsigned index() const;
    const std::string &name() const;
};
class Sound {
public:
    const std::shared_ptr<EffectsChain> &effectChain() const;
};
class Musician {
public:
    const std::shared_ptr<gp::core::Track> &coreTrack() const;
    std::shared_ptr<Sound> soundAtIndex(unsigned) const;
    void updateAll();
};
class Conductor {
public:
    const std::shared_ptr<gp::core::Score> &score() const;
    Musician *musician(unsigned) const;
    std::shared_ptr<Sound> sound(unsigned, unsigned) const;
};
class __declspec(dllimport) ConductorController {
public:
    const std::shared_ptr<Conductor> &conductor() const;
};
}
