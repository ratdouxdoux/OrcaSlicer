#include "AIKnowledgeBase.hpp"
#include "PrintConfig.hpp"

#include <boost/nowide/fstream.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <set>
#include <stdexcept>

namespace Slic3r::AIPrintAssistant {
namespace {
using json = nlohmann::json;

const std::vector<std::string> CATEGORIES = {"machine",  "troubleshooting", "maintenance", "calibration",
                                             "setting",  "material",        "software",    "safety"};
const std::vector<std::string> MACHINES   = {"u1", "j1", "artisan", "snapmaker2", "any"};
const std::vector<std::string> LISTS      = {"symptoms", "causes", "steps", "facts"};

bool contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// Plural and -ing/-ed folding so "clogged", "clogging" and "clogs" meet at "clog".
std::string stem(std::string word)
{
    auto ends = [&](const char* suffix) {
        const size_t n = std::char_traits<char>::length(suffix);
        return word.size() > n && word.compare(word.size() - n, n, suffix) == 0;
    };
    auto undouble = [&] {
        const size_t n = word.size();
        if (n >= 3 && word[n - 1] == word[n - 2] && !contains({"l", "s", "z"}, std::string(1, word[n - 1])))
            word.pop_back();
    };
    if (word.size() > 4 && ends("ies"))
        word.replace(word.size() - 3, 3, "y");
    else if (word.size() > 3 && ends("s") && !ends("ss") && !ends("us"))
        word.pop_back();
    // A vowel must remain ("oozing" -> "ooz", but "string" stays); "-eed" words keep their ending.
    if (word.size() >= 6 && ends("ing") && word.find_first_of("aeiouy") < word.size() - 3) {
        word.resize(word.size() - 3);
        undouble();
    } else if (word.size() >= 6 && ends("ed") && word[word.size() - 3] != 'e') {
        word.resize(word.size() - 2);
        undouble();
    }
    if (word.size() > 4 && ends("e"))
        word.pop_back();
    return word;
}

const std::set<std::string>& stopwords()
{
    static const std::set<std::string> words = {"a",    "an",   "and",  "are",  "as",   "at",   "be",   "by",   "can", "do",
                                                "does", "for",  "from", "how",  "i",    "if",   "in",   "into", "is",  "it",
                                                "my",   "of",   "on",   "or",   "the",  "this", "to",   "too",  "was", "what",
                                                "when", "why",  "with", "you",  "your", "after", "get", "got",  "has", "have"};
    return words;
}

// ASCII alphanumeric words; underscores and other punctuation separate them, so
// setting keys such as retraction_length index as "retraction" and "length".
std::vector<std::string> tokens(const std::string& text)
{
    std::vector<std::string> out;
    std::string              word;
    auto                     flush = [&] {
        if (word.size() >= 2 && !stopwords().count(word))
            out.push_back(stem(word));
        word.clear();
    };
    for (unsigned char c : text) {
        if (c < 128 && std::isalnum(c))
            word += char(std::tolower(c));
        else
            flush();
    }
    flush();
    return out;
}

void require_string(const json& entry, const char* key, size_t max_length, const std::string& where)
{
    if (!entry.contains(key) || !entry[key].is_string() || entry[key].get<std::string>().empty() ||
        entry[key].get<std::string>().size() > max_length)
        throw std::runtime_error(where + ": " + key + " must be a non-empty string of at most " + std::to_string(max_length) + " bytes.");
}

void require_strings(const json& value, size_t max_items, size_t max_length, const std::string& where)
{
    if (!value.is_array() || value.size() > max_items)
        throw std::runtime_error(where + " must be an array of at most " + std::to_string(max_items) + " items.");
    for (const auto& item : value)
        if (!item.is_string() || item.get<std::string>().empty() || item.get<std::string>().size() > max_length)
            throw std::runtime_error(where + " items must be non-empty strings of at most " + std::to_string(max_length) + " bytes.");
}

void validate_entry(const json& entry, std::set<std::string>& ids)
{
    if (!entry.is_object())
        throw std::runtime_error("Each knowledge entry must be an object.");
    const std::string where = "Knowledge entry " + (entry.contains("id") && entry["id"].is_string() ? entry["id"].get<std::string>() : "?");
    static const std::set<std::string> fields = {"id",     "title", "category", "machines", "tags",     "summary",
                                                 "symptoms", "causes", "steps",  "settings", "facts",    "sources"};
    for (auto it = entry.begin(); it != entry.end(); ++it)
        if (!fields.count(it.key()))
            throw std::runtime_error(where + ": unknown field " + it.key() + ".");
    require_string(entry, "id", 80, where);
    const auto id = entry["id"].get<std::string>();
    if (id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos || !ids.insert(id).second)
        throw std::runtime_error(where + ": id must be unique lowercase kebab-case.");
    require_string(entry, "title", 150, where);
    require_string(entry, "summary", 1000, where);
    if (!entry.contains("category") || !entry["category"].is_string() || !contains(CATEGORIES, entry["category"].get<std::string>()))
        throw std::runtime_error(where + ": unknown category.");
    if (!entry.contains("machines"))
        throw std::runtime_error(where + ": machines is required.");
    require_strings(entry["machines"], MACHINES.size(), 16, where + " machines");
    for (const auto& machine : entry["machines"])
        if (!contains(MACHINES, machine.get<std::string>()) || (machine == "any" && entry["machines"].size() > 1))
            throw std::runtime_error(where + ": machines must be u1, j1, artisan, snapmaker2, or any on its own.");
    if (entry["machines"].empty())
        throw std::runtime_error(where + ": machines must not be empty.");
    if (!entry.contains("tags"))
        throw std::runtime_error(where + ": tags is required.");
    require_strings(entry["tags"], 20, 60, where + " tags");
    for (const auto& list : LISTS)
        if (entry.contains(list))
            require_strings(entry[list], 20, 500, where + " " + list);
    if (entry.contains("settings")) {
        if (!entry["settings"].is_array() || entry["settings"].size() > 20)
            throw std::runtime_error(where + ": settings must be an array of at most 20 items.");
        for (const auto& setting : entry["settings"]) {
            if (!setting.is_object() || setting.size() != 2)
                throw std::runtime_error(where + ": settings items need exactly key and advice.");
            require_string(setting, "key", 80, where + " setting");
            require_string(setting, "advice", 400, where + " setting");
            if (print_config_def.get(setting["key"].get<std::string>()) == nullptr)
                throw std::runtime_error(where + ": unknown setting key " + setting["key"].get<std::string>() + ".");
        }
    }
    if (!entry.contains("sources") || !entry["sources"].is_array() || entry["sources"].empty() || entry["sources"].size() > 16)
        throw std::runtime_error(where + ": sources must list 1 to 16 sources.");
    for (const auto& source : entry["sources"]) {
        if (!source.is_object() || source.size() != 2)
            throw std::runtime_error(where + ": sources items need exactly title and url.");
        require_string(source, "title", 200, where + " source");
        require_string(source, "url", 500, where + " source");
        const auto url = source["url"].get<std::string>();
        if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
            throw std::runtime_error(where + ": source urls must be http(s).");
    }
}
} // namespace

std::string knowledge_machine(const std::string& printer_model)
{
    const auto model = lower(printer_model);
    if (model.find("snapmaker") == std::string::npos)
        return {};
    if (model.find("u1") != std::string::npos)
        return "u1";
    if (model.find("j1") != std::string::npos)
        return "j1";
    if (model.find("artisan") != std::string::npos)
        return "artisan";
    for (const char* family : {"a150", "a250", "a350"})
        if (model.find(family) != std::string::npos)
            return "snapmaker2";
    return {};
}

json knowledge_query_schema()
{
    return {{"type", "object"},
            {"properties",
             {{"query",
               {{"type", "string"},
                {"minLength", 1},
                {"maxLength", 200},
                {"description", "English keywords: symptoms, components, error codes, settings or materials."}}},
              {"machine",
               {{"type", "string"},
                {"enum", json::array({"u1", "j1", "artisan", "snapmaker2", "any", "all"})},
                {"description", "Defaults to the active printer plus machine-independent entries. any: machine-independent only; all: "
                                "every machine."}}},
              {"category", {{"type", "string"}, {"enum", CATEGORIES}}},
              {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}, {"default", 5}}}}},
            {"required", json::array({"query"})},
            {"additionalProperties", false}};
}

void validate_knowledge_query(const json& arguments)
{
    if (!arguments.is_object())
        throw std::runtime_error("Tool arguments must be an object.");
    for (auto it = arguments.begin(); it != arguments.end(); ++it)
        if (!contains({"query", "machine", "category", "limit"}, it.key()))
            throw std::runtime_error("Unexpected knowledge-search argument: " + it.key());
    if (!arguments.contains("query") || !arguments["query"].is_string() || tokens(arguments["query"].get<std::string>()).empty() ||
        arguments["query"].get<std::string>().size() > 200)
        throw std::runtime_error("query must contain searchable English keywords (at most 200 bytes).");
    if (arguments.contains("machine") &&
        (!arguments["machine"].is_string() || !contains({"u1", "j1", "artisan", "snapmaker2", "any", "all"}, arguments["machine"].get<std::string>())))
        throw std::runtime_error("machine must be u1, j1, artisan, snapmaker2, any or all.");
    if (arguments.contains("category") && (!arguments["category"].is_string() || !contains(CATEGORIES, arguments["category"].get<std::string>())))
        throw std::runtime_error("Unknown knowledge category.");
    if (arguments.contains("limit") &&
        (!arguments["limit"].is_number_integer() || arguments["limit"].get<int>() < 1 || arguments["limit"].get<int>() > 10))
        throw std::runtime_error("limit must be an integer from 1 to 10.");
}

KnowledgeBase::KnowledgeBase(const json& document)
{
    if (!document.is_object() || !document.contains("entries") || !document["entries"].is_array())
        throw std::runtime_error("The knowledge base must be an object with an entries array.");
    std::set<std::string> ids;
    // Titles and tags are what users and the model are most likely to name.
    const std::vector<std::pair<const char*, double>> weights = {{"title", 3.},  {"tags", 3.},  {"summary", 1.5}, {"symptoms", 1.5},
                                                                 {"causes", 1.}, {"steps", .7}, {"facts", 1.}};
    for (const auto& entry : document["entries"]) {
        validate_entry(entry, ids);
        Document indexed;
        auto     add = [&](const std::string& text, double weight) {
            for (const auto& token : tokens(text)) {
                indexed.terms[token] += weight;
                indexed.length += weight;
            }
        };
        for (const auto& [field, weight] : weights) {
            if (!entry.contains(field))
                continue;
            if (entry[field].is_string())
                add(entry[field].get<std::string>(), weight);
            else
                for (const auto& item : entry[field])
                    add(item.get<std::string>(), weight);
        }
        if (entry.contains("settings"))
            for (const auto& setting : entry["settings"])
                add(setting["key"].get<std::string>() + " " + setting["advice"].get<std::string>(), 1.);
        add(entry["category"].get<std::string>(), .5);
        for (const auto& term : indexed.terms)
            ++m_document_frequency[term.first];
        m_average_length += indexed.length;
        m_documents.push_back(std::move(indexed));
        m_entries.push_back(entry);
    }
    if (!m_documents.empty())
        m_average_length /= double(m_documents.size());
}

KnowledgeBase KnowledgeBase::load(const std::string& path)
{
    boost::nowide::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("The assistant knowledge base is missing.");
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.size() > 32 * 1024 * 1024)
        throw std::runtime_error("The assistant knowledge base is too large.");
    try {
        return KnowledgeBase(json::parse(text));
    } catch (const json::exception& error) {
        throw std::runtime_error(std::string("The assistant knowledge base is not valid JSON: ") + error.what());
    }
}

json KnowledgeBase::search(const json& arguments, const std::string& active_machine) const
{
    validate_knowledge_query(arguments);
    const auto machine  = arguments.value("machine", active_machine.empty() ? std::string("all") : active_machine);
    const auto category = arguments.value("category", std::string());
    const auto limit    = size_t(arguments.value("limit", 5));
    auto       query    = tokens(arguments["query"].get<std::string>());
    std::sort(query.begin(), query.end());
    query.erase(std::unique(query.begin(), query.end()), query.end());

    const double                      k1 = 1.2, b = .75, total = double(m_documents.size());
    std::vector<std::pair<double, size_t>> ranked;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const auto& entry    = m_entries[i];
        const auto& machines = entry["machines"];
        const bool  specific = std::find(machines.begin(), machines.end(), machine) != machines.end();
        const bool  general  = machines[0] == "any";
        if ((machine == "any" && !general) || (machine != "all" && machine != "any" && !specific && !general))
            continue;
        if (!category.empty() && entry["category"] != category)
            continue;
        double score = 0;
        for (const auto& term : query) {
            const auto found = m_documents[i].terms.find(term);
            if (found == m_documents[i].terms.end())
                continue;
            const double frequency = double(m_document_frequency.at(term));
            const double idf       = std::log(1. + (total - frequency + .5) / (frequency + .5));
            const double tf        = found->second;
            score += idf * tf * (k1 + 1.) / (tf + k1 * (1. - b + b * m_documents[i].length / std::max(1., m_average_length)));
        }
        // Prefer the user's own machine over equally relevant general advice.
        if (score > 0 && specific && machine != "all" && machine != "any")
            score *= 1.25;
        if (score > 0)
            ranked.emplace_back(score, i);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });

    json   results = json::array();
    size_t bytes   = 0;
    for (const auto& [score, index] : ranked) {
        if (results.size() >= limit)
            break;
        auto item  = m_entries[index];
        item["score"] = std::round(score * 100.) / 100.;
        bytes += item.dump().size();
        // Keep tool results small enough for the conversation; always return the best match.
        if (!results.empty() && bytes > 48 * 1024)
            break;
        results.push_back(std::move(item));
    }
    json out = {{"query", arguments["query"]},
                {"machine", machine},
                {"matches", ranked.size()},
                {"results", std::move(results)},
                {"note", "Entries summarize the cited sources. Live settings, slice results and the user's own observations take "
                         "precedence; cite the source URL when relying on an entry."}};
    if (ranked.empty())
        out["message"] = "No matching knowledge. Try other keywords, machine \"all\", or rely on live context.";
    return out;
}
} // namespace Slic3r::AIPrintAssistant
