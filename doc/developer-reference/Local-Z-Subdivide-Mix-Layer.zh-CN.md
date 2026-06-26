# Local-Z / Subdivide Mix Layer 中文说明

本文说明 `Subdivide Mix Layer` 如何为混色耗材生成对象子层，以及这些子层需要额外换料时，擦料塔是如何规划的。

相关代码：

- `src/libslic3r/PrintConfig.cpp`：面向用户的选项，例如 `dithering_local_z_mode`
- `src/libslic3r/MixedFilament.hpp` 和 `src/libslic3r/MixedFilament.cpp`：虚拟混色耗材行，以及到物理挤出机的解析
- `src/libslic3r/PrintObjectSlice.cpp`：Local-Z 规划器、遮罩生成、子层计划
- `src/libslic3r/GCode.cpp`：把普通挤出路径裁剪成 Local-Z pass，并输出这些 pass
- `src/libslic3r/Print.cpp`：Local-Z 换料在擦料塔上的预规划
- `src/libslic3r/GCode/WipeTower2.cpp`：擦料塔几何与换料 G-code 生成
- `src/libslic3r/GCode.hpp`：G-code 导出期间使用的 `WipeTowerIntegration` 状态

## 术语

- 物理耗材：真实的 1-based 耗材 ID，从 `1` 到物理耗材数量。
- 混色耗材：排在物理 ID 之后的虚拟 1-based 耗材 ID。例如有四个物理耗材时，第一个启用的混色行是 ID `5`。
- 标称层：普通切片得到的层，范围从 `layer.print_z - layer.height` 到 `layer.print_z`。
- Local-Z pass，或子层：一个 `SubLayerPlan`，表示标称层内部更小的 Z 区间。
- 混色遮罩：被绘制为混色虚拟耗材的 XY 多边形区域。
- 固定遮罩：已经绘制为物理耗材、并且在 Local-Z 启用时仍必须绑定到某个物理挤出机的区域。
- 基础遮罩：移除混色/固定遮罩后的层内剩余区域。它主要用于调试输出；G-code 裁剪阶段会从挤出路径和混色遮罩并集计算实际可打印的基础区域。

## 总体流程

1. 多材料分割为每一层、每个耗材生成遮罩。
2. Local-Z 规划器把混色遮罩转换为 `PrintObject` 上的 `LocalZInterval` 和 `SubLayerPlan` 记录。
3. G-code 导出读取这些计划，用 Local-Z 遮罩裁剪已有的外墙路径，以及可选的填充路径，并在每个子层 Z 高度用对应的子层流量高度输出裁剪片段。
4. 如果启用了擦料塔，擦料塔会预先规划这些 Local-Z pass 所需的额外换料。
5. G-code 导出期间，Local-Z 路径 pass 会在普通标称层对象循环之前运行。完成后，打印机回到标称层 Z，再正常打印该层的剩余内容。

Local-Z 不会在每个子层重新切网格。切片器先生成普通层和普通挤出路径，然后按 XY 遮罩裁剪这些路径，并为每个 Local-Z pass 修改其 Z 和流量高度。

## 规划器入口

Local-Z 计划由 `PrintObjectSlice.cpp` 中的 `apply_mm_segmentation()` 触发构建。

构建计划之前：

- 混色绘制分割由多材料分割代码生成；
- `apply_mixed_surface_indentation()` 可能会扩张或收缩混色遮罩；
- 当 `dithering_local_z_mode` 启用时，`apply_mixed_component_surface_offsets()` 会被跳过，因为 Local-Z 通过子层高度和工具顺序解析颜色；
- 当 `dithering_local_z_whole_objects` 启用时，整对象混色外墙遮罩会通过 `local_z_planner_segmentation_with_whole_object_mixed_wall()` 与绘制覆盖区域合并。

`build_local_z_plan()` 会先清除旧计划。除非满足以下条件，否则它会直接退出：

- 分割层数量与对象层数量一致；
- 至少存在一个物理耗材；
- `dithering_local_z_mode` 已启用，或者存在一个渐变混色行。

同层点彩行会被排除。它们使用 XY/路径分布，而不是 Local-Z 子层堆叠。因此 `build_local_z_plan()` 检测到活动的点彩行时会跳过。

## 子层高度选择

对每个标称层，规划器会记录：

- `LocalZInterval::z_lo`、`z_hi` 和 `base_height`；
- 是否存在混色绘制；
- 该 interval 的子层计划在共享 plan vector 中的起始位置。

然后规划器会找出该层上活动的混色行，并按绘制遮罩面积选出主导混色行。除非该层进入隔离多行模式，否则主导行会提供默认的 pass 高度模式。

高度边界来自：

- `mixed_filament_height_lower_bound`，至少钳制到 `0.01` mm；
- `mixed_filament_height_upper_bound`，至少钳制到 lower bound。

最终选出的 pass 高度总会经过清理，保证：

- 数值有限；
- 尽可能保持在下限/上限内；
- 总和回到标称层高度。

高度选择主要有四条路径。

## 显式 A/B 高度

如果设置了 `mixed_color_layer_height_a` 或 `mixed_color_layer_height_b`，规划器会在标称层内重复使用这些首选高度。最后一个 pass 会被调整来消耗剩余高度。如果结果无法适配高度边界，则回退到自动构建逻辑。

## 自动双色 Local-Z

对普通双组分行，规划器会把 `mix_b_percent` 转换为 lower bound 与 upper bound 之间的目标组分高度：

- 组分 A 目标高度 = lower + A percent * (upper - lower)
- 组分 B 目标高度 = lower + B percent * (upper - lower)

然后它会构建 A/B/A/B 交替序列。当只需要两个 pass 时，除非渐变行有意控制顺序，否则该顺序会在层间保持稳定。

`choose_local_z_start_with_component_a()` 会比较 pass 高度和目标 A/B 高度，并决定 pass 0 应该使用 A 还是 B。如果两种选择等价，它会使用该行的 cadence index，让等分层不会在层间无谓翻转。

## 渐变行

渐变行指 `gradient_enabled` 且两个组分不同的混色行。规划器会为该行构建连续的活动层区间。对区间内的每一层，规划器按层在区间中的位置对 `gradient_start` 到 `gradient_end` 做线性插值，并计算 A/B 目标高度。

渐变 Local-Z 使用固定的双 pass 模式。实现会有意把第一个 pass 分配给组分 B，把第二个 pass 分配给组分 A，并交换 pass 高度来匹配期望的渐变方向。这就是渐变处理看起来不同于普通 A/B cadence 路径的原因。

## 直接多色 Local-Z

当 `dithering_local_z_direct_multicolor` 启用、没有设置显式 A/B 高度，并且某个混色行有三个或更多物理组分时，规划器可以直接在所有组分之间分配子层。

组分列表来自 `gradient_component_ids`；权重来自 `gradient_component_weights`，没有设置时默认等权重。pass 高度构建器会把标称层切分成足够的高度 bin，以便在 lower/upper 边界内满足权重。如果启用了该行的 `local_z_max_sublayers` 上限，也会受其限制。

`build_local_z_direct_multicolor_sequence()` 接着为每个 pass 选择物理挤出机。它会为每个组分追踪跨层的高度误差累积，使微小的舍入/适配误差能在后续层中被校正。

## 多行同层

如果同一个标称层上有多个混色行活动，并且没有设置显式 A/B 高度，规划器可能会使用隔离多行模式。每个活动行都会得到自己的 pass 高度序列和 dependency group，因此独立绘制区域不会被强制套用同一个主导 cadence。

规划器会在每个 `SubLayerPlan` 上存储 `dependency_group` 和 `dependency_order`。G-code 导出和擦料塔预规划使用这些字段，在尽量减少换料的同时保持相关 Local-Z pass 的顺序。

## 构建 SubLayerPlan 记录

对每个切分 pass，规划器会创建一个 `SubLayerPlan`：

- `layer_id`：标称对象层索引；
- `pass_index`：该标称层内部的子 pass 顺序；
- `split_interval`：真实 Local-Z 子层时为 true；
- `z_lo` / `z_hi`：子层边界；
- `print_z`：该 pass 顶部 Z，通常等于 `z_hi`；
- `flow_height`：子层挤出高度；
- `painted_masks_by_extruder`：按 zero-based 物理挤出机索引的 XY 遮罩；
- `fixed_painted_masks_by_extruder`：参与整对象 Local-Z 处理的固定物理遮罩；
- `dependency_group` / `dependency_order`：分组多行 pass 的顺序约束。

pass 的目标物理挤出机按以下顺序选择：

1. 如果直接多色序列活动，则使用它；
2. 根据 active pair 和起始方向进行严格 A/B 分配；
3. 回退到带强制 height-weighted resolution 的 `MixedFilamentManager::resolve()`。

选中的遮罩会被追加到 `painted_masks_by_extruder[target - 1]`。

如果该层有混色绘制但只需要一个 pass，规划器仍会写入一个 `split_interval = false` 的单个 `SubLayerPlan`。这样可以保持 cadence 和调试状态一致，而不会要求 G-code 导出额外输出 micro-pass。

## 对象子层的 G-code 输出

`GCode::process_layer()` 会读取每个对象的 `local_z_intervals()` 和 `local_z_sublayer_plan()`。

对每个带有切分 Local-Z interval 的 layer-to-print：

1. 它根据 interval 的 split plans 构建 `LocalZPassBucket` 记录。
2. 混色遮罩会按 `LOCAL_Z_PERIMETER_MASK_EXPAND_MM`（`0.10` mm）扩张，使裁剪能捕捉遮罩边界附近的外墙片段。
3. 基础挤出会按混色遮罩并集裁剪，并使用较小的 `LOCAL_Z_BASE_MASK_EXPAND_MM`（`0.04` mm）保护距离。
4. 每个 Local-Z pass 会通过 `clip_extrusion_collection_for_local_z()` 把已有挤出集合裁剪到该 pass 的遮罩。
5. 裁剪后的路径会应用 `flow_height`。`mm3_per_mm` 按新高度与原路径高度的比例缩放。
6. 闭合环会变成裁剪后的开放片段。可行时，seam placer 会把它们重新锚定到普通 seam 附近。

外墙总是会参与。填充仅在 `dithering_local_z_infill` 启用时参与。

Local-Z pass buckets 会在普通层的挤出机循环之前输出：

- 打印机切换到 pass 挤出机；
- 移动到 `SubLayerPlan::print_z + z_offset`；
- 打印裁剪后的 pass 片段；
- 所有 Local-Z pass 完成后，回到标称层 Z；
- 常规标称层挤出机循环继续打印支撑、基础路径和任何非 Local-Z 内容。

如果擦料塔 wiping overrides 正在活动，该层的 Local-Z phase B 会被禁用，因为这些 override 路径目前还不是 Local-Z aware。

## Local-Z 下的擦料塔规划

擦料塔在 `Print::process()` 的擦料塔步骤中规划。`WipeTower2` 会以普通方式创建，带有所有物理挤出机参数和普通层工具顺序。

在为某层规划标称换料之前，代码会检查相同的 `print_z` 是否存在 Local-Z pass：

1. `collect_local_z_wipe_tower_toolchanges()` 扫描匹配对象层的 `SubLayerPlan` 记录。
2. 它使用与 G-code Local-Z 路径输出器相同的方式补偿遮罩。
3. 它通过 `layer_has_local_z_extrusions()` 验证每个 pass 遮罩是否真的与可打印外墙或启用的填充挤出相交。
4. 它创建 pass references，其中包含这些 Local-Z pass 所需的物理挤出机。
5. 它按子层 `print_z`、对象索引和 pass index 排序 pass references。
6. 如果每个 pass 都有 dependency group，则使用 dependency scheduler。否则，它按相同子层 `print_z` 分组，并使用 `LocalZOrderOptimizer::order_bucket_extruders()`。
7. 最终结果是一串 old-tool -> new-tool 的 Local-Z 换料序列。

每个 Local-Z 换料都会通过 `WipeTower2::plan_local_z_toolchange()` 加入擦料塔计划。它们与普通逐层换料分开存储在 `WipeTowerInfo::local_z_tool_changes` 中，但仍会计入 `toolchanges_depth()`，因此也会影响擦料塔深度。

Local-Z 换料预规划完成后，普通标称层换料会通过 `WipeTower2::plan_toolchange()` 规划。这意味着 Local-Z purge 会布置在同一个标称擦料塔层上，并且位于该层普通换料之前。

## 擦料塔 G-code 生成

`WipeTower2::generate()` 会产生两个结果 vector：

- `tool_changes`：普通标称层擦料塔换料；
- `local_z_tool_changes`：在标称对象循环之前消费的 Local-Z 换料。

对每个擦料塔层，`generate()` 会：

1. 调用 `plan_tower()`，让普通换料和 Local-Z 换料深度都计入最终擦料塔深度；
2. 先把 Local-Z 换料输出到 `local_z_layer_result`；
3. 再把普通换料输出到 `layer_result`；
4. 调用 `finish_layer()`，让稀疏填充/墙填满塔内剩余区域；
5. 把两个 vector 都存储到 `Print::m_wipe_tower_data`。

`WipeTowerIntegration` 会收到这两个 vector。在 Local-Z phase B 期间，`GCode.cpp` 会调用：

```cpp
m_wipe_tower->tool_change(*this, extruder_id, false, true, nominal_layer_z);
```

这里的 `true` 标记这是一个 Local-Z 换料。`WipeTowerIntegration` 随后尝试消费下一个预规划的 `local_z_tool_changes[layer_idx]` 条目。如果请求的新工具和当前工具与预规划条目匹配，它就输出对应擦料塔 G-code。purge 在标称擦料塔层 Z 上执行，然后对象打印移动到请求的子层 Z。

如果预规划序列不匹配，integration 会回退到运行时路径：

- 如果存在 reserve boxes，则使用一个保留的 Local-Z tower slot；
- 否则执行不带擦料塔 purge 的直接挤出机切换。

reserve-slot 路径是安全兜底；正常的 Local-Z 混色路径使用预规划的 `local_z_tool_changes`。

## 重要结果

- Local-Z 混色是在同一个 XY 遮罩内垂直堆叠物理颜色。它不会在喷嘴内部创建一种新的混合材料。
- 子层来自已有的标称层挤出路径。几何会被裁剪，流量高度会被修改；网格不会按子层重新切片。
- 擦料塔仍按标称层组织。Local-Z purge 是标称塔层上的额外换料 box，不是为每个对象子层创建单独的塔层。
- 启用擦料塔时，Local-Z phase B 在普通标称层对象循环之前运行，因此所有 micro-pass 换料都能先 purge 并打印，然后再打印该层剩余内容。
- 不启用擦料塔时，Local-Z 仍会输出 micro-pass，但换料是直接 `set_extruder()`。
- 手动混色 pattern 和同层点彩由其他分布路径处理，不属于 Local-Z eligible。

## 调试

当 Local-Z plan 被构建时，`export_local_z_plan_debug()` 会写出：

- `local-z-plan-obj-<id>.json`
- `local-z-plan-obj-<id>-layer-<layer>-pass-<pass>.svg`

JSON 会显示 interval 高度、子层数量、pass Z 范围、流量高度和遮罩数量。SVG 文件会显示基础遮罩以及每个挤出机的绘制遮罩。

有用的日志包括：

- `Local-Z plan start`
- `Local-Z interval`
- `Local-Z plan built`
- `Local-Z context`
- `Local-Z phase-b prepared`
- `Local-Z phase-b emitting`
- `Local-Z wipe tower preplan`
- `Wipe tower layer plan`
- `Local-Z toolchange emitted from preplanned wipe tower sequence`
