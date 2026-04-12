#include "nix/cmd/command.hh"

using namespace nix;

struct CmdEvalCache : NixMultiCommand
{
    CmdEvalCache()
        : NixMultiCommand("eval-cache", RegisterCommand::getCommandsFor({"eval-cache"}))
    {
    }

    std::string description() override
    {
        return "Inspect and debug the evaluation tracing cache.";
    }

    Category category() override
    {
        return catUtility;
    }
};

static auto rCmdEvalCache = registerCommand<CmdEvalCache>("eval-cache");
