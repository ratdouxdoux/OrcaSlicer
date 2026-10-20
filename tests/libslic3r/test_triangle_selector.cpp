#include <catch2/catch_test_macros.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include <tbb/task_arena.h>

#include <limits>

using namespace Slic3r;

TEST_CASE("Compact strict painting meshes retain triangle coordinates and T-joints", "[TriangleSelector][PaintPreparation]")
{
    indexed_triangle_set input;
    input.vertices = {Vec3f(0, 0, 0), Vec3f(2, 0, 0), Vec3f(0, 2, 0), Vec3f(2, 2, 0), Vec3f(10, 0, 0), Vec3f(12, 0, 0), Vec3f(10, 2, 0)};
    input.indices  = {Vec3i32(0, 1, 2), Vec3i32(1, 3, 2), Vec3i32(4, 5, 6)};
    TriangleMesh     mesh(input);
    TriangleSelector selector(mesh);
    selector.set_facet(2, EnforcerBlockerType::Extruder3);
    const auto transform = Transform3d::Identity();
    selector.select_patch(0,
                          TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(0.8f, 0.8f, 0.f), Vec3f(0.f, 0.f, 10.f), 0.4f,
                                                                              TriangleSelector::SPHERE, transform,
                                                                              TriangleSelector::ClippingPlane()),
                          EnforcerBlockerType::Extruder1, transform, true);
    const auto       annotation = selector.serialize();
    TriangleSelector restored(mesh);
    restored.deserialize(annotation, false);
    for (int state : {0, 1, 2, 3, 120}) {
        const auto reference = restored.get_facets_strict(EnforcerBlockerType(state));
        const auto compact   = restored.get_facets_strict_compact(EnforcerBlockerType(state));
        REQUIRE(compact.indices.size() == reference.indices.size());
        std::vector<bool> used(compact.vertices.size(), false);
        for (size_t i = 0; i < compact.indices.size(); ++i)
            for (int j = 0; j < 3; ++j) {
                REQUIRE(compact.indices[i](j) >= 0);
                REQUIRE(size_t(compact.indices[i](j)) < compact.vertices.size());
                CHECK(compact.vertices[compact.indices[i](j)] == reference.vertices[reference.indices[i](j)]);
                used[compact.indices[i](j)] = true;
            }
        CHECK(std::all_of(used.begin(), used.end(), [](bool value) { return value; }));
        if (state == 3)
            CHECK(compact.vertices.size() == 3);
        if (state == 2 || state == 120)
            CHECK(compact.vertices.empty());
    }
    CHECK(restored.serialize() == annotation);
}

TEST_CASE("Painted slab projections preserve face order across worker counts", "[TriangleSelector][PaintPreparation]")
{
    indexed_triangle_set patch;
    constexpr int        side = 24;
    for (int y = 0; y <= side; ++y)
        for (int x = 0; x <= side; ++x)
            patch.vertices.emplace_back(float(x), float(y), float((x % 4) + 2 * (y % 3)) * 0.125f);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            if ((x + y) % 3 == 0)
                continue; // Open painted patches with holes and touching vertices.
            const int a = y * (side + 1) + x;
            patch.indices.emplace_back(a, a + 1, a + side + 1);
            patch.indices.emplace_back(a + 1, a + side + 2, a + side + 1);
        }
    const std::vector<float> heights{0.f, 0.125f, 0.25f, 0.5f, 0.75f, 1.f};
    for (bool mirror : {false, true}) {
        Transform3d transform = Transform3d::Identity();
        if (mirror)
            transform.scale(Vec3d(-1., 1., -1.));
        auto project = [&] {
            std::pair<std::vector<Polygons>, std::vector<Polygons>> out;
            slice_mesh_slabs(patch, heights, transform, &out.first, &out.second, nullptr, [] {});
            return out;
        };
        tbb::task_arena serial(1), parallel(8);
        const auto      expected = serial.execute(project);
        for (int repeat = 0; repeat < 3; ++repeat)
            CHECK(parallel.execute(project) == expected);
    }
}

TEST_CASE("Ordinary mesh slices preserve serial face order across worker counts", "[MeshSlicing][SliceOrder]")
{
    indexed_triangle_set mesh;
    // Many disconnected, touching, open patches exercise greedy gap repair as
    // well as general intersections and edges exactly on a slicing plane.
    constexpr int side = 260;
    for (int y = 0; y <= side; ++y)
        for (int x = 0; x <= side; ++x)
            mesh.vertices.emplace_back(float(x), float(y), float((x % 4) + 2 * (y % 3)) * 0.125f);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            if ((x + y) % 7 == 0)
                continue;
            const int a = y * (side + 1) + x;
            mesh.indices.emplace_back(a, a + 1, a + side + 1);
            mesh.indices.emplace_back(a + 1, a + side + 2, a + side + 1);
        }
    const std::vector<float> heights{0.f, 0.125f, 0.2f, 0.25f, 0.5f, 0.75f, 1.f};
    for (bool mirror : {false, true}) {
        MeshSlicingParamsEx params;
        if (mirror)
            params.trafo.scale(Vec3d(-1., 1., 1.));
        auto            slice = [&] { return slice_mesh_ex(mesh, heights, params); };
        tbb::task_arena serial(1), parallel(8);
        const auto      expected = serial.execute(slice);
        REQUIRE(std::any_of(expected.begin(), expected.end(), [](const ExPolygons& layer) { return !layer.empty(); }));
        for (int repeat = 0; repeat < 4; ++repeat)
            CHECK(parallel.execute(slice) == expected);
    }
}

TEST_CASE("Triangle selector round-trips painted states above sixteen", "[TriangleSelector][MMUPaint]")
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
    };

    TriangleMesh mesh(its);
    TriangleSelector selector(mesh);

    constexpr int painted_state = 120;
    selector.set_facet(0, static_cast<EnforcerBlockerType>(painted_state));

    auto data = selector.serialize();
    REQUIRE_FALSE(data.triangles_to_split.empty());
    REQUIRE(data.used_states.size() > painted_state);
    CHECK(data.used_states[painted_state]);

    data.reset_used_states();
    data.update_used_states(size_t(data.triangles_to_split.front().bitstream_start_idx));
    CHECK(data.used_states[painted_state]);
    CHECK(TriangleSelector::has_facets(data, static_cast<EnforcerBlockerType>(painted_state)));

    TriangleSelector restored(mesh);
    restored.deserialize(data, true, static_cast<EnforcerBlockerType>(painted_state));
    CHECK(restored.has_facets(static_cast<EnforcerBlockerType>(painted_state)));
}

TEST_CASE("Triangle selector identifies unpainted and virtual filament states without changing them", "[TriangleSelector][MMUPaint]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(2.f, 0.f, 0.f), Vec3f(0.f, 2.f, 0.f)};
    its.indices  = {stl_triangle_vertex_indices(0, 1, 2)};
    TriangleMesh     mesh(its);
    TriangleSelector selector(mesh);
    const Vec3f      hit(0.25f, 0.25f, 0.f);

    CHECK(selector.get_state_at(hit, 0) == EnforcerBlockerType::NONE);
    for (int filament_id : {1, 5, 20, 120, 255}) {
        const auto state = static_cast<EnforcerBlockerType>(filament_id);
        selector.set_facet(0, state);
        const auto before = selector.serialize();
        CHECK(selector.get_state_at(hit, 0) == state);
        CHECK(selector.get_state_at(Vec3f::Zero(), 0) == state);
        CHECK(selector.get_state_at(Vec3f(1.f, 1.f, 0.f), 0) == state);
        CHECK(selector.serialize() == before);
    }

    CHECK_FALSE(selector.get_state_at(hit, -1));
    CHECK_FALSE(selector.get_state_at(hit, 1));
    CHECK_FALSE(selector.get_state_at(Vec3f(2.f, 2.f, 0.f), 0));
    CHECK_FALSE(selector.get_state_at(Vec3f(std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f), 0));
}

TEST_CASE("Triangle selector identifies individual brush subdivisions and restored assignments", "[TriangleSelector][MMUPaint]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(2.f, 0.f, 0.f), Vec3f(0.f, 2.f, 0.f)};
    its.indices  = {stl_triangle_vertex_indices(0, 1, 2)};
    TriangleMesh     mesh(its);
    TriangleSelector selector(mesh);
    const Vec3f      painted_hit(0.25f, 0.25f, 0.f);
    const Vec3f      unpainted_hit(1.5f, 0.1f, 0.f);
    const auto       mixed_state = static_cast<EnforcerBlockerType>(120);
    const auto       transform   = Transform3d::Identity();
    selector.select_patch(0,
                          TriangleSelector::SinglePointCursor::cursor_factory(painted_hit, Vec3f(0.f, 0.f, 10.f), 0.2f,
                                                                              TriangleSelector::SPHERE, transform,
                                                                              TriangleSelector::ClippingPlane()),
                          mixed_state, transform, true);

    REQUIRE(selector.num_facets(mixed_state) > 0);
    REQUIRE(selector.num_facets(EnforcerBlockerType::NONE) > 0);
    const auto before = selector.serialize();
    CHECK(selector.get_state_at(painted_hit, 0) == mixed_state);
    CHECK(selector.get_state_at(unpainted_hit, 0) == EnforcerBlockerType::NONE);
    CHECK(selector.serialize() == before);

    TriangleSelector restored(mesh);
    restored.deserialize(before);
    CHECK(restored.get_state_at(painted_hit, 0) == mixed_state);
    CHECK(restored.get_state_at(unpainted_hit, 0) == EnforcerBlockerType::NONE);

    EnforcerBlockerStateMap remap;
    for (size_t i = 0; i < remap.size(); ++i)
        remap[i] = static_cast<EnforcerBlockerType>(i);
    remap[120] = EnforcerBlockerType::Extruder5;
    restored.remap_triangle_state(remap);
    CHECK(restored.get_state_at(painted_hit, 0) == EnforcerBlockerType::Extruder5);
    restored.reset();
    CHECK(restored.get_state_at(painted_hit, 0) == EnforcerBlockerType::NONE);
}
