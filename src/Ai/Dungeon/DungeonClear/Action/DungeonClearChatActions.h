/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARCHATACTIONS_H
#define _PLAYERBOT_DUNGEONCLEARCHATACTIONS_H

#include "Action.h"

class PlayerbotAI;

class DcOnAction : public Action
{
public:
    DcOnAction(PlayerbotAI* botAI) : Action(botAI, "dc on") {}
    bool Execute(Event event) override;
};

class DcOffAction : public Action
{
public:
    DcOffAction(PlayerbotAI* botAI) : Action(botAI, "dc off") {}
    bool Execute(Event event) override;
};

class DcSkipAction : public Action
{
public:
    DcSkipAction(PlayerbotAI* botAI) : Action(botAI, "dc skip") {}
    bool Execute(Event event) override;
};

// Toggles the `dungeon clear paused` flag. When running (and not paused) it
// pauses — the tank stops navigating and behaves like `dc off` while keeping
// all boss/skip progress. When already paused it resumes on the same boss
// (refusing if a party member is dead, like `dc on`).
class DcPauseAction : public Action
{
public:
    DcPauseAction(PlayerbotAI* botAI) : Action(botAI, "dc pause") {}
    bool Execute(Event event) override;
};

class DcStatusAction : public Action
{
public:
    DcStatusAction(PlayerbotAI* botAI) : Action(botAI, "dc status") {}
    bool Execute(Event event) override;
};

class DcBossesAction : public Action
{
public:
    DcBossesAction(PlayerbotAI* botAI) : Action(botAI, "dc bosses") {}
    bool Execute(Event event) override;
};

class DcGoAction : public Action
{
public:
    DcGoAction(PlayerbotAI* botAI) : Action(botAI, "dc go") {}
    bool Execute(Event event) override;
};

#endif
