/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Tier-2 navmesh routing regression suite. Each scenario under t/fixtures/nav/
// is a frozen routing problem — (map, start, goal) + expected outcome — replayed
// against a real sliced navmesh through DcNavHarness. Every historical geometry
// bug (ledge target unreachable, under-map seam, jump-gap island, route ending
// short) becomes one scenario line here, the geometry twin of the decision
// fixtures.
//
// Client-derived map data is NEVER committed. The slice lives under a gitignored
// DC_MAPDATA_DIR/mmaps (produced by tools/slice_mapdata.py). With no slice the
// whole suite GTEST_SKIPs, so a clean checkout still builds and runs Tier 1.
//
// Scenario format: one flat JSON object per line (shared DcDecisionJson), keys:
//   name (str), map (uint), sx,sy,sz, tx,ty,tz (float),
//   expectReachable (bool, default true),
//   expectComplete  (bool, optional — assert route reaches the goal poly),
//   maxStepZ        (float, optional — assert no vertical pop exceeds it),
//   minPoints       (uint,  optional — assert the polyline has >= this many pts).

#include "gtest/gtest.h"
#include "NavHarness.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDecisionJson.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef DC_FIXTURE_DIR
#define DC_FIXTURE_DIR "."
#endif
#ifndef DC_MAPDATA_DIR
#define DC_MAPDATA_DIR "."
#endif

namespace
{
    namespace fs = std::filesystem;

    struct Scenario
    {
        std::string name;
        uint32_t    map = 0;
        float sx = 0, sy = 0, sz = 0, tx = 0, ty = 0, tz = 0;
        bool  expectReachable = true;
        bool  hasExpectComplete = false; bool expectComplete = false;
        bool  hasMaxStepZ = false;       float maxStepZ = 0.0f;
        bool  hasMinPoints = false;      uint32_t minPoints = 0;
    };

    std::vector<Scenario> LoadScenarios()
    {
        std::vector<Scenario> out;
        fs::path const dir = fs::path(DC_FIXTURE_DIR) / "nav";
        if (!fs::exists(dir))
            return out;
        for (auto const& entry : fs::directory_iterator(dir))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            std::ifstream in(entry.path());
            std::string line;
            while (std::getline(in, line))
            {
                auto const parsed = DcDecisionJson::Parse(line);
                if (!parsed)
                    continue;  // blank / comment / not an object
                auto const& m = *parsed;
                Scenario s;
                s.name = DcDecisionJson::GetStr(m, "name", entry.path().stem().string());
                s.map = DcDecisionJson::GetU(m, "map", 0);
                s.sx = DcDecisionJson::GetF(m, "sx", 0);
                s.sy = DcDecisionJson::GetF(m, "sy", 0);
                s.sz = DcDecisionJson::GetF(m, "sz", 0);
                s.tx = DcDecisionJson::GetF(m, "tx", 0);
                s.ty = DcDecisionJson::GetF(m, "ty", 0);
                s.tz = DcDecisionJson::GetF(m, "tz", 0);
                s.expectReachable = DcDecisionJson::GetB(m, "expectReachable", true);
                if ((s.hasExpectComplete = DcDecisionJson::Has(m, "expectComplete")))
                    s.expectComplete = DcDecisionJson::GetB(m, "expectComplete", false);
                if ((s.hasMaxStepZ = DcDecisionJson::Has(m, "maxStepZ")))
                    s.maxStepZ = DcDecisionJson::GetF(m, "maxStepZ", 0);
                if ((s.hasMinPoints = DcDecisionJson::Has(m, "minPoints")))
                    s.minPoints = DcDecisionJson::GetU(m, "minPoints", 0);
                out.push_back(s);
            }
        }
        return out;
    }
}

TEST(DcNavGeometry, ScenariosRouteAsExpected)
{
    fs::path const mmapsDir = fs::path(DC_MAPDATA_DIR) / "mmaps";
    if (!fs::exists(mmapsDir))
        GTEST_SKIP() << "no sliced map data at " << mmapsDir
                     << " — run tools/slice_mapdata.py (never committed)";

    std::vector<Scenario> const scenarios = LoadScenarios();
    if (scenarios.empty())
        GTEST_SKIP() << "no nav scenarios under " << (fs::path(DC_FIXTURE_DIR) / "nav");

    std::map<uint32_t, std::shared_ptr<dtNavMesh>> meshes;  // per-map cache
    uint32_t ran = 0;
    uint32_t skipped = 0;

    for (Scenario const& s : scenarios)
    {
        auto it = meshes.find(s.map);
        if (it == meshes.end())
            it = meshes.emplace(s.map, DcNavHarness::LoadMap(DC_MAPDATA_DIR, s.map)).first;
        std::shared_ptr<dtNavMesh> const& mesh = it->second;
        if (!mesh)
        {
            ++skipped;  // map not sliced — covered by another run
            continue;
        }

        DcNavHarness::RouteResult const r =
            DcNavHarness::Route(mesh.get(), s.sx, s.sy, s.sz, s.tx, s.ty, s.tz);
        ASSERT_TRUE(r.built) << s.name;
        ++ran;

        EXPECT_EQ(r.reachable, s.expectReachable)
            << s.name << ": reachable mismatch (" << r.failureReason << ")";
        if (s.hasExpectComplete)
            EXPECT_EQ(r.corridorComplete, s.expectComplete) << s.name << ": completeness";
        if (s.expectReachable && s.hasMaxStepZ)
            EXPECT_LE(r.maxStepZ, s.maxStepZ)
                << s.name << ": vertical pop " << r.maxStepZ
                << " exceeds " << s.maxStepZ << " (under-map / ledge seam?)";
        if (s.expectReachable && s.hasMinPoints)
            EXPECT_GE(r.pointCount, s.minPoints) << s.name << ": too few route points";
    }

    if (ran == 0)
        GTEST_SKIP() << "nav scenarios present but no matching map slices ("
                     << skipped << " scenarios skipped)";
}
