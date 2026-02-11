/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_VECTORMEMORYMGR_H
#define _PLAYERBOT_VECTORMEMORYMGR_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ObjectGuid.h"

class Player;
class WorldPosition;

class VectorMemoryMgr
{
public:
    static VectorMemoryMgr& instance()
    {
        static VectorMemoryMgr instance;
        return instance;
    }

    bool IsEnabled() const;

    void NotifyTravelTargetSet(Player* bot, WorldPosition const& startPos, WorldPosition const& endPos, uint32 zoneId);
    void RecordRouteTeleportFallback(Player* bot, WorldPosition const& startPos, WorldPosition const& endPos,
                                     uint32 zoneId, uint32 stuckAttempts);
    void EnsureQuestAvoidData(Player* bot, uint32 zoneId, uint8 level);
    void RecordQuestEvent(Player* bot, uint32 questId, uint32 zoneId, uint8 level, std::string const& eventName,
                          std::string const& reason = "");

    bool IsGridAvoided(ObjectGuid::LowType botGuidLow, uint32 mapId, uint8 gridX, uint8 gridY);
    bool ShouldAvoidQuest(ObjectGuid::LowType botGuidLow, uint32 zoneId, uint32 questId);
    void AddLocalAvoidGrid(ObjectGuid::LowType botGuidLow, uint32 mapId, uint8 gridX, uint8 gridY, uint8 weight = 10);

private:
    VectorMemoryMgr();
    ~VectorMemoryMgr();

    VectorMemoryMgr(const VectorMemoryMgr&) = delete;
    VectorMemoryMgr& operator=(const VectorMemoryMgr&) = delete;
    VectorMemoryMgr(VectorMemoryMgr&&) = delete;
    VectorMemoryMgr& operator=(VectorMemoryMgr&&) = delete;

    enum class JobType : uint8
    {
        UpsertRouteEpisode = 0,
        UpsertQuestEpisode = 1,
        QueryAvoidGrids = 2,
        QueryQuestAvoid = 3
    };

    struct Job
    {
        JobType type = JobType::UpsertRouteEpisode;
        uint32 botGuidLow = 0;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        uint32 questId = 0;
        uint32 tsMs = 0;
        uint32 stuckAttempts = 0;
        uint8 level = 0;
        uint8 startGridX = 0;
        uint8 startGridY = 0;
        uint8 endGridX = 0;
        uint8 endGridY = 0;
        bool teleportFallbackUsed = false;
        std::string eventName;
        std::string reason;
    };

    struct BotCache
    {
        uint32 lastRouteQueryMs = 0;
        uint32 lastQuestQueryMs = 0;
        std::unordered_map<uint32, std::unordered_map<uint16, uint8>> avoidGridsByMap;
        std::unordered_map<uint32, std::unordered_set<uint32>> avoidQuestsByZone;
    };

    static uint16 PackGrid(uint8 gridX, uint8 gridY);
    static std::string BuildRouteText(Job const& job);
    static std::string BuildQuestText(Job const& job);
    static std::string BuildBody(std::initializer_list<std::pair<std::string, std::string>> kvs);
    static bool ParseResponseHasOk(std::string const& responseBody);
    static std::vector<std::string> SplitLines(std::string const& text);

    void StartWorkerIfNeeded();
    void StopWorker();
    bool Enqueue(Job&& job);
    void WorkerMain();
    void ProcessJob(Job const& job);

    bool PostText(std::string const& path, std::string const& body, std::string& responseBody);
    void ProcessAvoidGridResponse(Job const& job, std::string const& responseBody);
    void ProcessQuestAvoidResponse(Job const& job, std::string const& responseBody);

    mutable std::mutex workerMutex;
    std::thread workerThread;
    std::atomic_bool workerRunning{false};
    std::atomic_bool stopWorkerRequested{false};

    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::queue<Job> jobs;
    static constexpr size_t maxQueueSize = 2000;

    std::mutex cacheMutex;
    std::unordered_map<uint32, BotCache> botCaches;

    std::mutex logMutex;
    uint32 lastQueueFullWarnMs = 0;
    uint32 lastRequestErrorWarnMs = 0;
};

#define sVectorMemoryMgr VectorMemoryMgr::instance()

#endif
