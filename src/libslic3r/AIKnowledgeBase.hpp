#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r::AIPrintAssistant {

// Machine family used by knowledge entries (u1, j1, artisan, snapmaker2), or
// empty for printers the knowledge base does not cover.
std::string    knowledge_machine(const std::string& printer_model);
nlohmann::json knowledge_query_schema();
void           validate_knowledge_query(const nlohmann::json& arguments);

// Curated Snapmaker and FDM printing knowledge: summaries of the Snapmaker wiki
// and other cited sources, shipped with the application. Search is local
// keyword ranking (BM25 over weighted fields); queries never leave the app.
class KnowledgeBase
{
public:
    // Throws std::runtime_error for a malformed document or entry.
    explicit KnowledgeBase(const nlohmann::json& document);
    static KnowledgeBase load(const std::string& path);

    size_t size() const { return m_entries.size(); }
    // Without a machine argument, results are limited to active_machine and
    // machine-independent entries.
    nlohmann::json search(const nlohmann::json& arguments, const std::string& active_machine = {}) const;

private:
    struct Document
    {
        std::unordered_map<std::string, double> terms;
        double                                  length = 0;
    };
    std::vector<nlohmann::json>             m_entries;
    std::vector<Document>                   m_documents;
    std::unordered_map<std::string, size_t> m_document_frequency;
    double                                  m_average_length = 0;
};

} // namespace Slic3r::AIPrintAssistant
