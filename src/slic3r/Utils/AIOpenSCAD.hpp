#ifndef slic3r_AIOpenSCAD_hpp_
#define slic3r_AIOpenSCAD_hpp_

#include <atomic>
#include <string>
#include <thread>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r::AIPrintAssistant {

// The OpenSCAD bundled next to the application, else an installed one. Empty if none.
boost::filesystem::path find_openscad();

// Runs generate_model code on a worker thread: OpenSCAD in a fresh temporary
// folder, without a console window, with a time limit and, on Windows, a memory
// limit. The code must already have passed check_openscad_code. The UI thread
// polls done() and then takes the result.
class OpenSCADRun
{
public:
    struct Result
    {
        bool           ok = false;
        std::string    error;    // Why there is no mesh.
        std::string    messages; // OpenSCAD's errors, warnings and echo output.
        TriangleMesh   mesh;
        nlohmann::json summary;
        std::string    view_png_base64; // Rendered view of the mesh; empty if rendering failed.
        double         seconds = 0.;
    };

    explicit OpenSCADRun(std::string code, boost::filesystem::path openscad = find_openscad());
    ~OpenSCADRun();
    OpenSCADRun(const OpenSCADRun&)            = delete;
    OpenSCADRun& operator=(const OpenSCADRun&) = delete;

    bool   done() const { return m_done.load(); }
    void   cancel() { m_cancel.store(true); }
    Result take();

private:
    void                    run(const std::string& code);
    boost::filesystem::path m_openscad;
    std::atomic<bool>       m_cancel{false}, m_done{false};
    Result                  m_result;
    std::thread             m_thread;
};

} // namespace Slic3r::AIPrintAssistant
#endif
