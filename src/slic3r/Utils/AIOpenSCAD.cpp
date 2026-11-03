#include "AIOpenSCAD.hpp"
#include "libslic3r/AIGeneratedModel.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
// As in MediaPlayCtrl.cpp: an earlier header defines pid_t, which Boost.Process declares.
#undef pid_t
#include <boost/process.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Slic3r::AIPrintAssistant {
namespace {
namespace fs = boost::filesystem;
namespace bp = boost::process;
using Clock  = std::chrono::steady_clock;

constexpr auto BUILD_TIME_LIMIT = std::chrono::seconds(90);
constexpr auto VIEW_TIME_LIMIT  = std::chrono::seconds(30);

std::string read_file(const fs::path& path, uintmax_t limit)
{
    boost::system::error_code error;
    const auto                size = fs::file_size(path, error);
    if (error)
        return {};
    if (size > limit)
        throw std::runtime_error("OpenSCAD wrote a file larger than expected.");
    boost::nowide::ifstream file(path.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::string& text)
{
    boost::nowide::ofstream file(path.string(), std::ios::binary);
    file << text;
    if (!file)
        throw std::runtime_error("Could not write the OpenSCAD input file.");
}

#ifdef _WIN32
// Closing the job kills OpenSCAD if it still runs; the job also caps its memory.
struct Job
{
    HANDLE handle = CreateJobObjectW(nullptr, nullptr);
    explicit Job(bp::child& child)
    {
        if (!handle)
            return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        limits.ProcessMemoryLimit               = SIZE_T(4) << 30;
        SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(handle, child.native_handle());
    }
    ~Job()
    {
        if (handle)
            CloseHandle(handle);
    }
};
#endif

// Returns OpenSCAD's exit code and its messages, which it prints to stderr. Throws when the user
// stops the assistant or the time limit passes.
int execute(const fs::path&                 openscad,
            const std::vector<std::string>& arguments,
            const fs::path&                 directory,
            Clock::duration                 limit,
            const std::atomic<bool>&        cancel,
            std::string&                    log)
{
    const auto      log_path = directory / "openscad.log";
    std::error_code error;
#ifdef _WIN32
    bp::child child(openscad, bp::args = arguments, bp::start_dir = directory, bp::std_in < bp::null, bp::std_out > bp::null,
                    bp::std_err > log_path, bp::windows::create_no_window, error);
    Job       job(child);
#else
    bp::child child(openscad, bp::args = arguments, bp::start_dir = directory, bp::std_in < bp::null, bp::std_out > bp::null,
                    bp::std_err > log_path, error);
#endif
    if (error)
        throw std::runtime_error("OpenSCAD could not be started: " + error.message());
    const auto deadline = Clock::now() + limit;
    while (child.running(error)) {
        if (cancel.load() || Clock::now() > deadline) {
            child.terminate(error);
            child.wait(error);
            if (cancel.load())
                throw std::runtime_error("Stopped by the user.");
            throw std::runtime_error("OpenSCAD did not finish within " +
                                     std::to_string(std::chrono::duration_cast<std::chrono::seconds>(limit).count()) +
                                     " seconds. Simplify the design: lower $fn, and avoid minkowski, hull and long loops over many "
                                     "parts.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    child.wait(error);
    log = read_file(log_path, 4 * 1024 * 1024);
    return child.exit_code();
}
} // namespace

fs::path find_openscad()
{
    std::vector<fs::path>     candidates;
    boost::system::error_code error;
    const auto                program = boost::dll::program_location(error);
#ifdef _WIN32
    if (!error)
        candidates.push_back(program.parent_path() / "openscad" / "openscad.exe");
    for (const char* variable : {"ProgramW6432", "ProgramFiles"})
        if (const char* folder = boost::nowide::getenv(variable))
            candidates.push_back(fs::path(folder) / "OpenSCAD" / "openscad.exe");
#else
    if (!error)
        candidates.push_back(program.parent_path() / "openscad" / "openscad");
#ifdef __APPLE__
    candidates.push_back("/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD");
#endif
#endif
    for (const auto& candidate : candidates)
        if (fs::is_regular_file(candidate, error))
            return candidate;
    return bp::search_path("openscad");
}

OpenSCADRun::OpenSCADRun(std::string code, fs::path openscad)
    : m_openscad(std::move(openscad)), m_thread([this, code = std::move(code)] { run(code); })
{}

OpenSCADRun::~OpenSCADRun()
{
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}

OpenSCADRun::Result OpenSCADRun::take()
{
    if (m_thread.joinable())
        m_thread.join();
    return std::move(m_result);
}

void OpenSCADRun::run(const std::string& code)
{
    const auto start = Clock::now();
    Result     result;
    fs::path   directory;
    try {
        const auto& openscad = m_openscad;
        if (openscad.empty())
            throw std::runtime_error("OpenSCAD is not installed with this copy of Snapmaker Orca, so models cannot be generated from "
                                     "code. Use the native model tools instead.");
        directory = fs::temp_directory_path() / "snapmaker-orca-openscad" / fs::unique_path("%%%%-%%%%-%%%%-%%%%");
        fs::create_directories(directory);
        write_file(directory / "model.scad", code);
        std::string log;
        const int status = execute(openscad, {"-o", "model.stl", "--summary", "geometry", "--summary-file", "summary.json", "model.scad"},
                                   directory, BUILD_TIME_LIMIT, m_cancel, log);
        result.messages    = openscad_messages(log, directory.generic_string());
        if (status != 0 || !fs::exists(directory / "model.stl"))
            throw std::runtime_error("OpenSCAD could not build the model. Fix the code using openscad_output and call generate_model "
                                     "again.");
        if (fs::file_size(directory / "model.stl") > 512 * 1024 * 1024)
            throw std::runtime_error("The model is too detailed. Lower $fn or simplify the design.");
        if (!result.mesh.ReadSTLFile((directory / "model.stl").string().c_str(), true))
            throw std::runtime_error("The mesh from OpenSCAD could not be read.");
        try {
            result.summary = nlohmann::json::parse(read_file(directory / "summary.json", 1024 * 1024));
        } catch (const std::exception&) {
            result.summary = nlohmann::json::object();
        }
        // A rendered view of the mesh as imported. It needs OpenGL, so it is optional.
        try {
            write_file(directory / "view.scad", "import(\"model.stl\");\n");
            std::string view_log;
            if (execute(openscad,
                        {"-o", "view.png", "--render", "--imgsize=512,512", "--viewall", "--autocenter", "--colorscheme=Tomorrow",
                         "view.scad"},
                        directory, VIEW_TIME_LIMIT, m_cancel, view_log) == 0) {
                const auto png = read_file(directory / "view.png", 2 * 1024 * 1024);
                result.view_png_base64.resize(boost::beast::detail::base64::encoded_size(png.size()));
                result.view_png_base64.resize(boost::beast::detail::base64::encode(result.view_png_base64.data(), png.data(), png.size()));
            }
        } catch (const std::exception&) {
            if (m_cancel.load())
                throw;
            result.view_png_base64.clear();
        }
        result.ok = true;
    } catch (const std::exception& error) {
        result.ok    = false;
        result.error = error.what();
        result.mesh  = TriangleMesh();
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    if (!directory.empty()) {
        // Windows keeps OpenSCAD's log locked for a moment after the process exits.
        boost::system::error_code error;
        for (int attempt = 0; attempt < 25; ++attempt) {
            fs::remove_all(directory, error);
            if (!error)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    m_result = std::move(result);
    m_done.store(true);
}

} // namespace Slic3r::AIPrintAssistant
