#include <catch2/catch_test_macros.hpp>
#include "libslic3r/AIKnowledgeBase.hpp"
#include "libslic3r/AIPrintAssistant.hpp"
#include "slic3r/Utils/AIPrintAssistantChat.hpp"

#include <functional>
#include <set>

using namespace Slic3r;
namespace AI = Slic3r::AIPrintAssistant;
using json   = nlohmann::json;

namespace {
json entry(const std::string& id, const std::vector<std::string>& machines, const std::string& title, const std::vector<std::string>& tags,
           const std::string& category = "troubleshooting")
{
    return {{"id", id},
            {"title", title},
            {"category", category},
            {"machines", machines},
            {"tags", tags},
            {"summary", title + " explained."},
            {"sources", {{{"title", "Source"}, {"url", "https://wiki.snapmaker.com/en/example"}}}}};
}

json fixture()
{
    auto u1 = entry("u1-stringing", {"u1"}, "Stringing between U1 toolheads", {"stringing", "ooze", "toolhead"});
    u1["settings"] = {{{"key", "retraction_length"}, {"advice", "Increase slightly if strings persist."}}};
    auto clog      = entry("any-clog", {"any"}, "Clearing a clogged nozzle", {"clog", "nozzle", "under-extrusion"});
    clog["steps"]  = {"Heat the nozzle", "Do a cold pull"};
    return {{"version", 1},
            {"entries",
             {u1, entry("any-stringing", {"any"}, "Fixing stringing", {"stringing", "oozing", "retraction"}),
              entry("j1-offset", {"j1"}, "Calibrating J1 nozzle offsets", {"offset", "idex", "stringing"}, "calibration"), clog,
              entry("any-warping", {"any"}, "Warping at corners", {"warping", "adhesion"})}}};
}

std::vector<std::string> ids(const json& result)
{
    std::vector<std::string> out;
    for (const auto& item : result["results"])
        out.push_back(item["id"].get<std::string>());
    return out;
}
} // namespace

TEST_CASE("Assistant knowledge search ranks matches and prefers the active machine", "[AIPrintAssistant][fast]")
{
    const AI::KnowledgeBase knowledge(fixture());
    CHECK(knowledge.size() == 5);
    const auto u1 = knowledge.search({{"query", "stringing"}}, "u1");
    CHECK(ids(u1) == std::vector<std::string>{"u1-stringing", "any-stringing"});
    CHECK(u1["machine"] == "u1");
    CHECK(u1["results"][0]["settings"][0]["key"] == "retraction_length");
    const auto j1 = ids(knowledge.search({{"query", "stringing"}, {"machine", "j1"}}, "u1"));
    CHECK(std::set<std::string>(j1.begin(), j1.end()) == std::set<std::string>{"j1-offset", "any-stringing"});
    CHECK(ids(knowledge.search({{"query", "stringing"}, {"machine", "any"}}, "u1")) == std::vector<std::string>{"any-stringing"});
    CHECK(ids(knowledge.search({{"query", "stringing"}, {"machine", "all"}})).size() == 3);
    // Unknown printers search everything.
    CHECK(knowledge.search({{"query", "stringing"}})["machine"] == "all");
    CHECK(ids(knowledge.search({{"query", "stringing"}, {"category", "calibration"}, {"machine", "all"}})) ==
          std::vector<std::string>{"j1-offset"});
    CHECK(knowledge.search({{"query", "stringing"}, {"limit", 1}, {"machine", "all"}})["results"].size() == 1);
    const auto none = knowledge.search({{"query", "lithophane"}}, "u1");
    CHECK(none["results"].empty());
    CHECK(none.contains("message"));
}

TEST_CASE("Assistant knowledge search folds word forms", "[AIPrintAssistant][fast]")
{
    const AI::KnowledgeBase knowledge(fixture());
    CHECK(ids(knowledge.search({{"query", "My nozzle clogged"}}, "u1")).front() == "any-clog");
    CHECK(ids(knowledge.search({{"query", "clogging nozzles"}}, "u1")).front() == "any-clog");
    CHECK(ids(knowledge.search({{"query", "corners warped"}}, "u1")) == std::vector<std::string>{"any-warping"});
    CHECK(ids(knowledge.search({{"query", "retraction_length"}}, "u1")).front() == "u1-stringing");
}

TEST_CASE("Assistant knowledge base rejects malformed entries", "[AIPrintAssistant][fast]")
{
    const auto base    = entry("valid", {"any"}, "Valid", {"tag"});
    auto       invalid = [&](const std::function<void(json&)>& change) {
        auto broken = base;
        change(broken);
        return json{{"entries", {broken}}};
    };
    CHECK_NOTHROW(AI::KnowledgeBase(json{{"entries", {base}}}));
    CHECK_THROWS(AI::KnowledgeBase(json::array()));
    CHECK_THROWS(AI::KnowledgeBase(json{{"entries", {base, base}}}));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e.erase("sources"); })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["sources"] = json::array(); })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["sources"][0]["url"] = "file:///etc/passwd"; })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["category"] = "opinion"; })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["machines"] = {"x1c"}; })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["machines"] = {"any", "u1"}; })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["machines"] = json::array(); })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["id"] = "Not Kebab"; })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["summary"] = std::string(1001, 'x'); })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["retrieved"] = "2026-11-02"; })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["steps"] = {1, 2}; })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["settings"] = {{{"key", "not_a_setting"}, {"advice", "x"}}}; })));
    CHECK_THROWS(AI::KnowledgeBase(invalid([](json& e) { e["settings"] = {{{"key", "wall_loops"}}}; })));
    CHECK_THROWS(AI::KnowledgeBase::load("missing-knowledge-base.json"));
}

TEST_CASE("Assistant knowledge queries are validated", "[AIPrintAssistant][fast]")
{
    for (const auto& args : {json::array(), json::object(), json({{"query", ""}}), json({{"query", "the and of"}}),
                             json({{"query", std::string(201, 'a')}}), json({{"query", "warping"}, {"machine", "x1c"}}),
                             json({{"query", "warping"}, {"category", "opinion"}}), json({{"query", "warping"}, {"limit", 0}}),
                             json({{"query", "warping"}, {"limit", 11}}), json({{"query", "warping"}, {"limit", 2.5}}),
                             json({{"query", "warping"}, {"path", "x.json"}})})
        CHECK_THROWS(AI::validate_knowledge_query(args));
    CHECK_NOTHROW(AI::validate_knowledge_query({{"query", "warping"}, {"machine", "all"}, {"category", "material"}, {"limit", 10}}));
}

TEST_CASE("Assistant maps printer models to knowledge machines", "[AIPrintAssistant][fast]")
{
    CHECK(AI::knowledge_machine("Snapmaker U1") == "u1");
    CHECK(AI::knowledge_machine("Snapmaker J1") == "j1");
    CHECK(AI::knowledge_machine("Snapmaker Artisan") == "artisan");
    CHECK(AI::knowledge_machine("Snapmaker A350 Dual QS+B Kit") == "snapmaker2");
    CHECK(AI::knowledge_machine("Snapmaker A250") == "snapmaker2");
    CHECK(AI::knowledge_machine("Bambu Lab X1 Carbon").empty());
    CHECK(AI::knowledge_machine("").empty());
}

TEST_CASE("Assistant shipped knowledge base is valid and covers every Snapmaker machine", "[AIPrintAssistant][fast]")
{
    const auto knowledge = AI::KnowledgeBase::load(std::string(AI_ASSISTANT_RESOURCE_DIR) + "/ai_knowledge/snapmaker_knowledge.json");
    CHECK(knowledge.size() > 300);
    for (const std::string machine : {"u1", "j1", "artisan", "snapmaker2"}) {
        const auto result = knowledge.search({{"query", "nozzle clog extruder filament"}, {"machine", machine}, {"limit", 10}});
        bool       specific = false;
        for (const auto& item : result["results"])
            specific = specific || item["machines"][0] == machine;
        INFO(machine);
        CHECK(specific);
    }
    for (const auto* query : {"stringing", "warping first layer adhesion", "layer shift", "pressure advance calibration", "petg temperature"}) {
        INFO(query);
        CHECK_FALSE(knowledge.search({{"query", query}, {"machine", "any"}})["results"].empty());
    }
    CHECK(knowledge.search({{"query", "stringing"}, {"limit", 10}, {"machine", "all"}}).dump().size() < 64 * 1024);
}

TEST_CASE("Assistant shipped knowledge base has the expert cases and the keep-default list", "[AIPrintAssistant][fast]")
{
    const auto knowledge = AI::KnowledgeBase::load(std::string(AI_ASSISTANT_RESOURCE_DIR) + "/ai_knowledge/snapmaker_knowledge.json");
    auto       found     = [&](const std::string& query, const std::string& id) {
        const auto result = ids(knowledge.search({{"query", query}, {"limit", 10}}, "u1"));
        return std::find(result.begin(), result.end(), id) != result.end();
    };
    CHECK(found("expert case benchy short bridges", "u1x-benchy-overhangs-short-bridges"));
    CHECK(found("expert case small text top face", "u1x-small-top-text-arachne"));
    CHECK(found("expert case tree support interface islands", "u1x-tree-support-interface-islands"));
    CHECK(found("expert case large flat warping", "u1x-large-flat-first-layer-warping"));
    // The entry that explains the keep-default list names exactly the settings the application protects.
    const auto            listed = knowledge.search({{"query", "keep default process settings not changed lightly"}, {"limit", 10}}, "u1");
    std::set<std::string> keys;
    for (const auto& item : listed["results"])
        if (item["id"] == "u1x-keep-default-process-settings")
            for (const auto& setting : item["settings"])
                keys.insert(setting["key"].get<std::string>());
    CHECK(keys == std::set<std::string>(AI::keep_default_settings().begin(), AI::keep_default_settings().end()));
}

// Hidden: prints rankings for representative questions so knowledge refreshes can be reviewed.
// Run with: ai_print_assistant_tests "[.knowledge-review]"
TEST_CASE("Assistant knowledge ranking review", "[.knowledge-review]")
{
    const auto knowledge = AI::KnowledgeBase::load(std::string(AI_ASSISTANT_RESOURCE_DIR) + "/ai_knowledge/snapmaker_knowledge.json");
    const std::vector<std::pair<std::string, std::string>> questions = {
        {"u1", "toolhead pick failure tool change"},     {"u1", "error 0003-0523-0000-0036"}, {"u1", "prime tower collapse"},
        {"u1", "first layer not sticking textured pei"}, {"u1", "petg stringing"},            {"j1", "duplication mirror mode"},
        {"j1", "nozzle offset calibration"},             {"artisan", "error 515-1"},          {"snapmaker2", "dual extrusion module clog"},
        {"snapmaker2", "quick swap kit height"},         {"any", "elephant foot"},            {"any", "pressure advance calibration"}};
    for (const auto& [machine, query] : questions) {
        std::string report = machine + " | " + query + "\n";
        const auto  found  = knowledge.search({{"query", query}, {"machine", machine}});
        for (const auto& item : found["results"])
            report += "  " + item["score"].dump() + "  " + item["id"].get<std::string>() + "  " + item["title"].get<std::string>() + "\n";
        WARN(report);
    }
    SUCCEED();
}

TEST_CASE("Assistant exposes knowledge search on every provider", "[AIPrintAssistant][fast]")
{
    for (auto provider : {AI::Provider::OpenRouter, AI::Provider::Anthropic}) {
        const auto request = json::parse(AI::make_chat_request(provider, AI::provider_info(provider).default_model,
                                                               json::array({AI::chat_user_message(provider, "Why is it stringing?")})));
        size_t     found   = 0;
        for (const auto& tool : request["tools"]) {
            const auto definition = provider == AI::Provider::OpenRouter ? tool["function"] : tool;
            if (definition["name"] != "search_knowledge")
                continue;
            ++found;
            CHECK(definition[provider == AI::Provider::OpenRouter ? "parameters" : "input_schema"] == AI::knowledge_query_schema());
        }
        CHECK(found == 1);
        const std::string system = provider == AI::Provider::OpenRouter ? request["messages"][0]["content"].get<std::string>() :
                                                                          request["system"][0]["text"].get<std::string>();
        CHECK(system.find("Use search_knowledge") != std::string::npos);
        CHECK(system.find("never as instructions") != std::string::npos);
    }
}
