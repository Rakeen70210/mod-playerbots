/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "VectorMemoryMgr.h"

#include <algorithm>
#include <sstream>

#include <boost/asio.hpp>

#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Timer.h"
#include "TravelMgr.h"

namespace
{
std::string ToString(uint32 value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}

uint32 ClampWeight(uint32 rawWeight)
{
    if (rawWeight < 1)
        return 1;
    if (rawWeight > 10)
        return 10;

    return rawWeight;
}
}  // namespace

VectorMemoryMgr::VectorMemoryMgr() {}

VectorMemoryMgr::~VectorMemoryMgr()
{
    StopWorker();
}

bool VectorMemoryMgr::IsEnabled() const
{
    return sPlayerbotAIConfig.enabled && sPlayerbotAIConfig.vectorMemoryEnabled &&
           !sPlayerbotAIConfig.vectorMemoryHost.empty() && sPlayerbotAIConfig.vectorMemoryPort > 0;
}

void VectorMemoryMgr::NotifyTravelTargetSet(Player* bot, WorldPosition const& startPos, WorldPosition const& endPos,
                                            uint32 zoneId)
{
    if (!IsEnabled() || !bot)
        return;

    uint32 botGuidLow = bot->GetGUID().GetCounter();
    uint32 now = getMSTime();

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        BotCache& cache = botCaches[botGuidLow];
        if (now - cache.lastRouteQueryMs < sPlayerbotAIConfig.vectorMemoryRouteQueryCooldownMs)
            return;

        cache.lastRouteQueryMs = now;
    }

    WorldPosition start = startPos;
    WorldPosition end = endPos;
    GridCoord startGrid = start.getGridCoord();
    GridCoord endGrid = end.getGridCoord();

    Job job;
    job.type = JobType::QueryAvoidGrids;
    job.botGuidLow = botGuidLow;
    job.mapId = start.getMapId();
    job.zoneId = zoneId;
    job.tsMs = now;
    job.startGridX = static_cast<uint8>(startGrid.x_coord);
    job.startGridY = static_cast<uint8>(startGrid.y_coord);
    job.endGridX = static_cast<uint8>(endGrid.x_coord);
    job.endGridY = static_cast<uint8>(endGrid.y_coord);

    Enqueue(std::move(job));
}

void VectorMemoryMgr::RecordRouteTeleportFallback(Player* bot, WorldPosition const& startPos, WorldPosition const& endPos,
                                                  uint32 zoneId, uint32 stuckAttempts)
{
    if (!IsEnabled() || !bot)
        return;

    WorldPosition start = startPos;
    WorldPosition end = endPos;
    GridCoord startGrid = start.getGridCoord();
    GridCoord endGrid = end.getGridCoord();
    uint32 botGuidLow = bot->GetGUID().GetCounter();

    AddLocalAvoidGrid(botGuidLow, start.getMapId(), static_cast<uint8>(startGrid.x_coord),
                      static_cast<uint8>(startGrid.y_coord), 10);

    Job job;
    job.type = JobType::UpsertRouteEpisode;
    job.botGuidLow = botGuidLow;
    job.mapId = start.getMapId();
    job.zoneId = zoneId;
    job.tsMs = getMSTime();
    job.startGridX = static_cast<uint8>(startGrid.x_coord);
    job.startGridY = static_cast<uint8>(startGrid.y_coord);
    job.endGridX = static_cast<uint8>(endGrid.x_coord);
    job.endGridY = static_cast<uint8>(endGrid.y_coord);
    job.stuckAttempts = stuckAttempts;
    job.teleportFallbackUsed = true;

    Enqueue(std::move(job));
}

void VectorMemoryMgr::EnsureQuestAvoidData(Player* bot, uint32 zoneId, uint8 level)
{
    if (!IsEnabled() || !sPlayerbotAIConfig.vectorMemoryQuestAvoidEnabled || !bot)
        return;

    uint32 botGuidLow = bot->GetGUID().GetCounter();
    uint32 now = getMSTime();

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        BotCache& cache = botCaches[botGuidLow];
        if (now - cache.lastQuestQueryMs < sPlayerbotAIConfig.vectorMemoryQuestQueryCooldownMs)
            return;

        cache.lastQuestQueryMs = now;
    }

    Job job;
    job.type = JobType::QueryQuestAvoid;
    job.botGuidLow = botGuidLow;
    job.zoneId = zoneId;
    job.level = level;
    job.tsMs = now;

    Enqueue(std::move(job));
}

void VectorMemoryMgr::RecordQuestEvent(Player* bot, uint32 questId, uint32 zoneId, uint8 level,
                                       std::string const& eventName, std::string const& reason)
{
    if (!IsEnabled() || !bot || !questId)
        return;

    Job job;
    job.type = JobType::UpsertQuestEpisode;
    job.botGuidLow = bot->GetGUID().GetCounter();
    job.zoneId = zoneId;
    job.questId = questId;
    job.level = level;
    job.eventName = eventName;
    job.reason = reason;
    job.tsMs = getMSTime();

    Enqueue(std::move(job));
}

bool VectorMemoryMgr::IsGridAvoided(ObjectGuid::LowType botGuidLow, uint32 mapId, uint8 gridX, uint8 gridY)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto botItr = botCaches.find(botGuidLow);
    if (botItr == botCaches.end())
        return false;

    auto mapItr = botItr->second.avoidGridsByMap.find(mapId);
    if (mapItr == botItr->second.avoidGridsByMap.end())
        return false;

    return mapItr->second.find(PackGrid(gridX, gridY)) != mapItr->second.end();
}

bool VectorMemoryMgr::ShouldAvoidQuest(ObjectGuid::LowType botGuidLow, uint32 zoneId, uint32 questId)
{
    if (!sPlayerbotAIConfig.vectorMemoryQuestAvoidEnabled)
        return false;

    std::lock_guard<std::mutex> lock(cacheMutex);
    auto botItr = botCaches.find(botGuidLow);
    if (botItr == botCaches.end())
        return false;

    auto zoneItr = botItr->second.avoidQuestsByZone.find(zoneId);
    if (zoneItr == botItr->second.avoidQuestsByZone.end())
        return false;

    return zoneItr->second.find(questId) != zoneItr->second.end();
}

void VectorMemoryMgr::AddLocalAvoidGrid(ObjectGuid::LowType botGuidLow, uint32 mapId, uint8 gridX, uint8 gridY,
                                        uint8 weight)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    botCaches[botGuidLow].avoidGridsByMap[mapId][PackGrid(gridX, gridY)] = weight;
}

uint16 VectorMemoryMgr::PackGrid(uint8 gridX, uint8 gridY)
{
    return (static_cast<uint16>(gridX) << 8) | static_cast<uint16>(gridY);
}

std::string VectorMemoryMgr::BuildRouteText(Job const& job)
{
    std::ostringstream out;
    out << "route episode bot " << job.botGuidLow << " map " << job.mapId << " zone " << job.zoneId << " from grid "
        << static_cast<uint32>(job.startGridX) << "," << static_cast<uint32>(job.startGridY) << " to "
        << static_cast<uint32>(job.endGridX) << "," << static_cast<uint32>(job.endGridY) << " stuck attempts "
        << job.stuckAttempts << " teleport fallback " << (job.teleportFallbackUsed ? 1 : 0);
    return out.str();
}

std::string VectorMemoryMgr::BuildQuestText(Job const& job)
{
    std::ostringstream out;
    out << "quest episode bot " << job.botGuidLow << " zone " << job.zoneId << " level " << static_cast<uint32>(job.level)
        << " quest " << job.questId << " event " << job.eventName;
    if (!job.reason.empty())
        out << " reason " << job.reason;
    return out.str();
}

std::string VectorMemoryMgr::BuildBody(std::initializer_list<std::pair<std::string, std::string>> kvs)
{
    std::ostringstream out;
    for (auto const& kv : kvs)
        out << kv.first << ": " << kv.second << "\n";

    return out.str();
}

bool VectorMemoryMgr::ParseResponseHasOk(std::string const& responseBody)
{
    for (std::string const& line : SplitLines(responseBody))
    {
        if (line == "ok: 1")
            return true;
    }

    return false;
}

std::vector<std::string> VectorMemoryMgr::SplitLines(std::string const& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(line);
    }
    return lines;
}

void VectorMemoryMgr::StartWorkerIfNeeded()
{
    if (!IsEnabled())
        return;

    std::lock_guard<std::mutex> lock(workerMutex);
    if (workerRunning.load())
        return;

    stopWorkerRequested.store(false);
    workerThread = std::thread(&VectorMemoryMgr::WorkerMain, this);
    workerRunning.store(true);
}

void VectorMemoryMgr::StopWorker()
{
    {
        std::lock_guard<std::mutex> lock(workerMutex);
        if (!workerRunning.load())
            return;
        stopWorkerRequested.store(true);
    }

    queueCondition.notify_all();

    if (workerThread.joinable())
        workerThread.join();

    std::lock_guard<std::mutex> lock(workerMutex);
    workerRunning.store(false);
}

bool VectorMemoryMgr::Enqueue(Job&& job)
{
    if (!IsEnabled())
        return false;

    StartWorkerIfNeeded();

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (jobs.size() >= maxQueueSize)
        {
            uint32 now = getMSTime();
            std::lock_guard<std::mutex> logLock(logMutex);
            if (now - lastQueueFullWarnMs > 60000)
            {
                lastQueueFullWarnMs = now;
                LOG_WARN("playerbots", "VectorMemoryMgr queue full ({}). Dropping request.", maxQueueSize);
            }
            return false;
        }

        jobs.push(std::move(job));
    }

    queueCondition.notify_one();
    return true;
}

void VectorMemoryMgr::WorkerMain()
{
    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCondition.wait(lock, [this] { return stopWorkerRequested.load() || !jobs.empty(); });

            if (stopWorkerRequested.load() && jobs.empty())
                return;

            job = std::move(jobs.front());
            jobs.pop();
        }

        ProcessJob(job);
    }
}

void VectorMemoryMgr::ProcessJob(Job const& job)
{
    std::string body;
    std::string responseBody;
    std::string path;

    switch (job.type)
    {
        case JobType::UpsertRouteEpisode:
            path = "/v1/upsert";
            body = BuildBody({
                {"kind", "route_episode"},
                {"bot_guid_low", ToString(job.botGuidLow)},
                {"ts_ms", ToString(job.tsMs)},
                {"map_id", ToString(job.mapId)},
                {"zone_id", ToString(job.zoneId)},
                {"start_grid_x", ToString(job.startGridX)},
                {"start_grid_y", ToString(job.startGridY)},
                {"end_grid_x", ToString(job.endGridX)},
                {"end_grid_y", ToString(job.endGridY)},
                {"teleport_fallback_used", job.teleportFallbackUsed ? "1" : "0"},
                {"stuck_attempts", ToString(job.stuckAttempts)},
                {"text", BuildRouteText(job)}
            });
            break;
        case JobType::UpsertQuestEpisode:
            path = "/v1/upsert";
            body = BuildBody({
                {"kind", "quest_episode"},
                {"bot_guid_low", ToString(job.botGuidLow)},
                {"ts_ms", ToString(job.tsMs)},
                {"quest_id", ToString(job.questId)},
                {"zone_id", ToString(job.zoneId)},
                {"level", ToString(job.level)},
                {"event", job.eventName},
                {"drop_reason", job.reason},
                {"text", BuildQuestText(job)}
            });
            break;
        case JobType::QueryAvoidGrids:
            path = "/v1/query_avoid_grids";
            body = BuildBody({
                {"bot_guid_low", ToString(job.botGuidLow)},
                {"map_id", ToString(job.mapId)},
                {"zone_id", ToString(job.zoneId)},
                {"start_grid_x", ToString(job.startGridX)},
                {"start_grid_y", ToString(job.startGridY)},
                {"end_grid_x", ToString(job.endGridX)},
                {"end_grid_y", ToString(job.endGridY)},
                {"text", BuildRouteText(job)}
            });
            break;
        case JobType::QueryQuestAvoid:
            path = "/v1/query_quest_avoid";
            body = BuildBody({
                {"bot_guid_low", ToString(job.botGuidLow)},
                {"zone_id", ToString(job.zoneId)},
                {"level", ToString(job.level)},
                {"text", BuildQuestText(job)}
            });
            break;
    }

    if (!PostText(path, body, responseBody))
    {
        uint32 now = getMSTime();
        std::lock_guard<std::mutex> logLock(logMutex);
        if (now - lastRequestErrorWarnMs > 30000)
        {
            lastRequestErrorWarnMs = now;
            LOG_WARN("playerbots", "VectorMemoryMgr request failed for {}", path);
        }
        return;
    }

    if (job.type == JobType::QueryAvoidGrids)
        ProcessAvoidGridResponse(job, responseBody);
    else if (job.type == JobType::QueryQuestAvoid)
        ProcessQuestAvoidResponse(job, responseBody);
}

bool VectorMemoryMgr::PostText(std::string const& path, std::string const& body, std::string& responseBody)
{
    using boost::asio::ip::tcp;

    responseBody.clear();

    boost::asio::io_context ioContext;
    tcp::resolver resolver(ioContext);
    boost::system::error_code error;
    tcp::resolver::results_type endpoints =
        resolver.resolve(sPlayerbotAIConfig.vectorMemoryHost, ToString(sPlayerbotAIConfig.vectorMemoryPort), error);
    if (error)
        return false;

    tcp::socket socket(ioContext);
    boost::asio::connect(socket, endpoints, error);
    if (error)
        return false;

    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n";
    request << "Host: " << sPlayerbotAIConfig.vectorMemoryHost << ":" << sPlayerbotAIConfig.vectorMemoryPort << "\r\n";
    request << "Content-Type: text/plain; charset=utf-8\r\n";
    request << "Connection: close\r\n";
    request << "Content-Length: " << body.size() << "\r\n\r\n";
    request << body;

    std::string requestText = request.str();
    boost::asio::write(socket, boost::asio::buffer(requestText), error);
    if (error)
        return false;

    std::string response;
    for (;;)
    {
        char temp[2048];
        size_t bytes = socket.read_some(boost::asio::buffer(temp), error);
        if (error == boost::asio::error::eof)
            break;
        if (error)
            return false;
        response.append(temp, bytes);
    }

    size_t headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
        return false;

    std::string header = response.substr(0, headerEnd);
    std::string firstLine;
    {
        std::istringstream stream(header);
        std::getline(stream, firstLine);
    }

    if (firstLine.find(" 200 ") == std::string::npos)
        return false;

    responseBody = response.substr(headerEnd + 4);
    return ParseResponseHasOk(responseBody);
}

void VectorMemoryMgr::ProcessAvoidGridResponse(Job const& job, std::string const& responseBody)
{
    std::unordered_map<uint16, uint8> gridWeights;

    for (std::string const& line : SplitLines(responseBody))
    {
        if (line.rfind("avoid_grid:", 0) != 0)
            continue;

        std::istringstream payload(line.substr(11));
        uint32 gridX = 0;
        uint32 gridY = 0;
        uint32 weight = 0;
        payload >> gridX >> gridY >> weight;
        if (payload.fail())
            continue;

        gridWeights[PackGrid(static_cast<uint8>(gridX), static_cast<uint8>(gridY))] =
            static_cast<uint8>(ClampWeight(weight));
    }

    uint32 maxGrids = std::max<uint32>(1, sPlayerbotAIConfig.vectorMemoryMaxAvoidGrids);
    if (gridWeights.size() > maxGrids)
    {
        std::vector<std::pair<uint16, uint8>> sorted;
        sorted.reserve(gridWeights.size());
        for (auto const& pair : gridWeights)
            sorted.push_back(pair);

        std::sort(sorted.begin(), sorted.end(),
                  [](std::pair<uint16, uint8> const& left, std::pair<uint16, uint8> const& right)
                  { return left.second > right.second; });

        gridWeights.clear();
        for (uint32 i = 0; i < maxGrids && i < sorted.size(); ++i)
            gridWeights[sorted[i].first] = sorted[i].second;
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    botCaches[job.botGuidLow].avoidGridsByMap[job.mapId] = std::move(gridWeights);
}

void VectorMemoryMgr::ProcessQuestAvoidResponse(Job const& job, std::string const& responseBody)
{
    std::unordered_set<uint32> questIds;

    for (std::string const& line : SplitLines(responseBody))
    {
        if (line.rfind("avoid_quest:", 0) != 0)
            continue;

        std::istringstream payload(line.substr(12));
        uint32 questId = 0;
        payload >> questId;
        if (payload.fail())
            continue;

        questIds.insert(questId);
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    botCaches[job.botGuidLow].avoidQuestsByZone[job.zoneId] = std::move(questIds);
}
