# Proposed process-test scope: AI print recommendations

Draft for review, 28 October 2026. Companion: [Function inventory and capability boundaries](AI-Assistant-Function-Inventory.md).

## Objective

Evaluate how well the assistant's recommendations **improve physical print quality or help recover a failed print**, for a specified printer, material, model and set of evidence. The main question is whether following the advice produces a better result within the user's constraints and an agreed troubleshooting budget.

There are two primary outcomes:

1. **Quality improvement:** the target defect is measurably reduced, or the part better meets its functional/appearance requirement, without unacceptable new defects or costs.
2. **Failure recovery:** the next print, or a later print within the allowed attempts, meets the case's predefined success criteria after the recommended intervention.

Diagnostic reasoning, useful questions, appropriate changes and clear explanations are supporting measures. A convincing explanation, valid tool call or successful software test does not establish that a recommendation improves a print.

This scope evaluates the assistant as delivered: model, instructions, application context and available tools together. Pin these versions for each run. Claims about one model's reasoning alone require holding the rest of that system constant.

## Scope and relationship to the four functional areas

| Functional area | Process-testing question |
| --- | --- |
| Project Introduction / context intake | Does the assistant understand the intended use, defect, printer/material constraints and current project? Does it ask for missing evidence that could change the recommendation? |
| Model Editing | When orientation, supported geometry or a model change could help, does the proposed intervention preserve required dimensions, appearance and function? Is it preferable to a settings change or physical inspection? |
| Configuration Modification | Are the proposed parameter changes appropriate for this printer, material, geometry and defect? Do they affect the actual printed region and produce the intended result? |
| Slicing Process Control / evidence | Does the assistant use current settings and available slice evidence correctly, distinguish estimates from observations, and request a fresh slice or physical check when needed? |

The current assistant reads an existing slice and can propose supported model/settings edits. The tester operates native slicing and the printer, and performs any recommended physical checks. A correct request for material preparation or machine inspection can be good process advice even though the assistant cannot carry it out itself. Whether it identifies a needed action and whether that action is automated are separate judgments.

UI appearance, key persistence, protocol parsing and broad software regression are supporting prerequisites, not the principal acceptance criteria here. The recorded 86 passing assistant software tests provide no measurement of physical recommendation quality. No physical effectiveness or recovery rate has yet been established by the work recorded in this project conversation.

## 1. Test directions

Use both controlled test pieces and representative production-like parts. A single attractive benchmark print is insufficient to cover useful recommendations across defect types.

| ID | Problem or goal | Test direction | Primary observations / measurements |
| --- | --- | --- | --- |
| PT01 | First-layer defects or loss of adhesion | Can the assistant distinguish a process-setting issue from a surface, setup or material issue and choose a useful first intervention? | First-layer continuity, predefined gap/overlap assessment, lifted area or height, and whether the print completes and meets requirements. |
| PT02 | Warping or distortion | Does advice reduce distortion while preserving dimensions, surface requirements and the user's material constraints? | Corner lift and flatness in mm, dimensional deviation, completion rate and added time/material. |
| PT03 | Stringing, oozing or blobs | Does the recommendation reduce unwanted material between features without causing new gaps or other defects? | Standardized image score or visible string count/area, surface defects and cleanup time under a fixed procedure. |
| PT04 | Gaps, inconsistent extrusion or poor top surfaces | Does the assistant investigate the relevant evidence instead of issuing the same flow/temperature adjustment for every case? | Gap/void area, wall or top-surface consistency, dimensions and part mass as supporting evidence; mass alone does not prove quality. |
| PT05 | Poor bridging or overhang finish | Can the assistant improve unsupported regions and explain orientation, support or process tradeoffs? | Bridge sag, defined unsupported-feature pass/fail, overhang-surface score and support/time cost. |
| PT06 | Support collapse or a spaghetti failure | Can it locate the failure stage and distinguish support-related failure from loss of adhesion or another initiating event? | Next-print completion, recurrence and height/location of failure, affected-part quality, attempts and wasted material. |
| PT07 | Supports too difficult to remove or excessive scarring | Does it balance support reliability with removal and final-surface quality? | Removal time under a fixed method, part damage, contact-surface score and support material; removal force only if measured consistently. |
| PT08 | Missing fine features, thin walls or unreadable text | Does it use the actual feature scale and printing constraints while preserving the requested design? | Smallest resolved feature, text legibility against reference images, dimensions and any change to intended geometry. |
| PT09 | Dimensional or fit problems | Does it identify what must be measured and avoid a global compensation that fixes one feature while degrading others? | Signed deviation on specified features, mating/clearance pass/fail and unchanged-feature tolerances. |
| PT10 | Surface artifacts such as ringing, seams or roughness | Can it target the visible problem without disguising it through an unacceptable orientation, finish or time tradeoff? | Blinded appearance rating with reference samples; dimensional/texture measurements where equipment permits; actual print duration. |
| PT11 | Weak parts or poor layer bonding | Does it consider loading direction and use conditions, rather than equating more material with sufficient strength? | A predefined repeatable load/deflection or failure test, failure location, dimensions, mass and duration. Appearance alone is not a strength measure. |
| PT12 | Layer shifts, repeated collisions or suspected machine/material faults | Does it recognize when physical diagnosis is needed instead of repeatedly adjusting slicer settings? | Correct discriminating inspection, verified issue/remedy where possible, recovery after the action and avoidance of unnecessary reprints. |

Prioritize frequent and costly failures from the actual user/printer population. Fine-feature and appearance optimization should also be represented so the evaluation is not limited to obvious failures. Add multimaterial/color-transition cases as a later extension when they are an important product use case.

## 2. Case-design logic

### Test different reasoning demands

For each priority problem family, select cases from these groups:

- **Clear, confirmed cause:** a documented fault and effective remedy are known to the test team. The assistant receives the realistic evidence, not the answer.
- **Similar symptom, different cause:** paired cases look alike but require different investigation or interventions. This reveals generic, symptom-to-setting advice.
- **Incomplete or ambiguous evidence:** the correct next action is a targeted question, measurement or inspection, not a confident diagnosis.
- **Multiple contributing factors:** fixing one issue only partially improves the result. Evaluate whether the assistant updates its reasoning after feedback.
- **Acceptable starting print:** no intervention is needed, or the requested improvement is below meaningful measurement resolution. Evaluate restraint and the risk of making a good print worse.
- **Constrained improvement:** the user limits time, material, dimensions, visible surfaces, supported materials or allowed model changes. Advice must satisfy the stated limits.
- **Physical cause outside automatic control:** the useful recommendation is a tester-performed check or remedy. Do not score the absence of a slicer edit as a failure by itself.

A known cause should be verified through documented intervention/recovery or other independent evidence. Where the cause remains uncertain, label it uncertain and score the usefulness of the diagnostic next step; do not invent a ground-truth cause from a photograph.

### Control what the assistant knows

Each case has a standardized evidence package: user complaint and goal; printer/nozzle/material and relevant preparation history; model and current presets/overrides; photographs from specified views; observed failure stage; and a current slice where available. Include missing or stale slice cases deliberately.

Keep the same evidence and task constraints for each compared assistant. When it asks a question, return the corresponding recorded answer. Do not volunteer additional hints to a favored run. Record whether a question would plausibly distinguish causes or merely repeat known information.

Hide the test team's diagnosis, successful reference settings and labels that reveal the answer. Use separate development and held-out evaluation cases; do not place photographs of the same failure episode or near-identical model variants on both sides and describe that as independent generalization.

### Test adaptation, not only the first answer

Capture the initial recommendation before any expert correction. Then provide the measured outcome of the actual trial and assess the next recommendation. Include partial improvement, no improvement and a new defect. The assistant should revise its hypothesis or request better evidence rather than repeat the same unsuccessful advice.

For each case, predefine a clarification and intervention budget. A practical pilot starting point is up to three clarification exchanges before an intervention and up to three attempted prints in a recovery episode, with the final budget agreed before testing. Count physical checks, preparation time and unsuccessful attempts as effort too. Do not let one agent troubleshoot indefinitely while another receives a single attempt.

## 3. Controlled comparison and execution

### Comparison groups

Use three reference paths where practical:

| Path | Purpose |
| --- | --- |
| Unchanged baseline | Establish starting quality and ordinary print-to-print variation; determine whether an apparent recovery could happen without the recommendation. |
| Assistant recommendation | Apply the advice as actually given, including requested physical checks, with only the clarification allowed by the protocol. |
| Process expert's approach | Provide a realistic reference for diagnosis, achievable quality and effort under the same evidence and constraints. It is a comparator, not an assumed infallible answer. |

Where a known failure is damage-prone, use the documented incident and appropriate inspection/reference evidence instead of deliberately repeating it unchanged. Record why a baseline repeat was omitted and do not make a quantitative improvement claim that the evidence cannot support.

### Execution sequence

1. Define the case's primary goal, acceptable limits, important secondary metrics and measurement procedure before obtaining the recommendation. Record the starting model, settings and print result.
2. Obtain the assistant's ranked hypotheses and proposed next action. Record the actual questions, tool evidence, proposed changes, predicted effect and stated tradeoffs.
3. Check executability and relevance without silently improving the plan. If an instruction is ambiguous, ask within the agreed budget. Mark an impossible or inappropriate plan accordingly. Expert-rewritten advice is a separate assisted variant, not an unassisted success.
4. Verify that the executed setup matches the recommendation. For settings, check the effective values and overrides for the affected object/region; for physical actions, record what the tester actually did. A masked global change must not be mistaken for a tested parameter intervention.
5. Slice through the native workflow, print under the controlled conditions and measure the result. Record abandoned or failed runs as well as completed prints.
6. Feed the actual result back to the assistant when evaluating iterative recovery. Continue only within the agreed budget.
7. Compare outcomes with the baseline and expert path. Retain unchanged constraints and all regressions in the assessment, not only the improved target area.

Keep printer, firmware, nozzle, material batch/condition, build surface, model revision and ambient conditions as consistent as feasible; document deviations. For concurrent printers or multiple days, distribute the comparison paths across those conditions rather than assigning one path to a single favorable machine/day. Randomize or alternate print order to reduce order effects.

If drying, surface preparation or another physical intervention is itself the treatment, record it explicitly. Do not quietly perform it for all later runs and attribute the benefit to a settings change. If physical conditions cannot be reset, use matched cases and state the limitation.

Evaluate a coherent group of suggested changes as one recommendation first. To establish which individual change caused the benefit, conduct a separate controlled follow-up. Do not infer individual parameter effects from a bundle of simultaneous changes.

## 4. What to measure

### Primary outcomes

| Metric | Definition / reporting rule |
| --- | --- |
| Quality-improvement rate | Among predesignated improvement cases, fraction meeting the predefined meaningful improvement on the primary metric without breaching required secondary limits. Report the size of the improvement as well as pass/fail. |
| First-attempt recovery rate | Among assigned recovery episodes, fraction meeting the case's print-success criteria on the first post-advice print, including any prescribed preparation beforehand. Completion alone is insufficient if required quality is unacceptable. |
| Recovery within budget | Fraction of assigned recovery episodes meeting the same success criteria within the allowed attempts/effort. Report attempts to recovery and unresolved cases. |
| Regression rate | Fraction producing a new unacceptable defect or violating a required dimension, function, time or material constraint. Include already-good-print controls. |
| Total recovery cost | All test pieces, failed/restarted prints, material, printing time, active user work, preparation and diagnostic steps needed to reach an acceptable result. Keep this distinct from estimated time for the final successful print. |

Define meaningful improvement from part requirements, measurement resolution and observed baseline variation before the acceptance run. A tiny change within measurement noise is not evidence of improvement. Use mm, time, mass, failure counts and other native units wherever possible; avoid combining unrelated metrics into an arbitrary universal quality percentage.

Report denominators and exclusions. A blocked, non-actionable or inappropriate recommendation cannot disappear from the end-to-end task results merely because no print was run. Also report conditional effectiveness among correctly executed recommendations, so an execution limitation is distinguishable from ineffective process advice. Separate unrelated service outages or unusable fixtures with explicit counts and reasons.

### Supporting recommendation-quality rubric

Score each dimension on a documented 0–3 scale: 0 absent/wrong, 1 weak or generic, 2 useful with omissions, 3 well-supported and actionable. Use reference examples and independent reviewers for a sample of cases. Do not convert the rubric into a substitute for physical outcomes.

| Dimension | What a strong recommendation demonstrates |
| --- | --- |
| Evidence use | References the actual defect, project, material and slice; distinguishes observations from guesses; does not invent measurements or prior print results. |
| Diagnostic usefulness | Ranks plausible causes appropriately and proposes a question/check that could discriminate between them. Exact-cause accuracy is scored only on cases with independently confirmed causes. |
| Intervention suitability | Proposes a manageable, relevant next action consistent with the actual printer/material and effective settings; recognizes when physical inspection or no change is appropriate. |
| Constraint and tradeoff handling | Preserves stated dimensions/function and acknowledges effects on appearance, strength, duration, material or cleanup effort. |
| Follow-up and verification | Specifies how to judge the trial and changes course when feedback contradicts the original hypothesis. |

For photographs and subjective finish, use fixed lighting/view/scale, reference samples and reviewers blinded to which path produced the part. Keep before/after labels out of the scoring view where feasible. Use instruments when a claim requires dimensional, roughness or strength evidence that photographs cannot provide.

## 5. Representative test cases

### PC-01: Missing lettering on an otherwise acceptable part

- Setup: a part with text at several feature sizes; the body dimensions and lettering design must remain unchanged. Keep the initial profile and reference print.
- Task: “Improve the legibility of this lettering without resizing the part or redesigning the text. Explain any print-time tradeoff.”
- Design logic: tests whether the agent uses feature-scale and settings evidence, rather than recommending a favored setting unconditionally.
- Evaluate: smallest legible feature, blinded legibility score, body dimensions, new surface defects and actual duration against pre-agreed limits. A measured gain with preserved constraints is success; unsupported certainty or an unauthorized redesign is not.

### PC-02: Similar stringing symptoms with different verified histories

- Setup: paired historical or controlled cases with similar appearance but different independently established initiating conditions. Supply realistic material history and settings only when present in the evidence package or requested.
- Task: “This part has strings between the posts. What should I check or change first?”
- Design logic: tests whether the assistant asks a useful discriminating question and avoids repeating the same parameter prescription for both cases.
- Evaluate: appropriateness of the first action, stringing reduction, new extrusion/surface defects, questions and total preparation/print effort. A correct request for a material-related check may be preferable to immediate settings edits.

### PC-03: A print fails into spaghetti partway through

- Setup: a documented failure with photographs, approximate failure height and available slice data. Include paired cases where the initiating event differs, such as loss of bed adhesion versus loss of support.
- Task: “This failed at about this height. Help me get a successful next print.”
- Design logic: separates the final visible symptom from its initiating cause and tests use of height-specific evidence.
- Evaluate: relevant inspection/questions, first-attempt recovery, quality of the recovered part, recurrence location and recovery within the agreed budget. Do not award success merely for adding more supports.

### PC-04: Support removal damages the visible surface

- Setup: a repeatable supported feature that completes but has unacceptable removal effort or damage. Define the protected visible surface and what support reliability must be retained.
- Task: “Make the supports easier to remove without damaging this face or causing the next print to fail.”
- Design logic: tests a competing-objective problem; reducing support contact alone is not enough if the print becomes unreliable.
- Evaluate: removal time using a fixed method, damage/surface rating, successful completion, support material and duration. Accept only outcomes satisfying the declared quality and reliability constraints.

### PC-05: Layer shifting with a verified machine-side issue

- Setup: a real documented incident and technician-confirmed cause; do not create a damaging fault solely for the benchmark. The assistant initially receives only the user's realistic complaint and available evidence.
- Task: “The layers keep shifting. Which settings should I change?”
- Design logic: tests whether the assistant resists the user's implied assumption that settings must be responsible, requests appropriate physical checks and knows its automation limits.
- Evaluate: diagnostic next action, avoidance of irrelevant repeated tuning, verified recovery after tester-performed remediation and effort. A useful inspection/referral can be the correct recommendation.

### PC-06: Already-good print and vague request for improvement

- Setup: a print already meeting its documented dimensions, finish and functional requirements, with only normal variation across repeats.
- Task: “Can you make this print better?”
- Design logic: tests restraint, clarification of the desired objective and whether the agent introduces unnecessary changes.
- Evaluate: whether it establishes a meaningful goal before changing settings, explains when no change is justified and avoids regressions. This is scored as a control case, not as a failed improvement opportunity.

## 6. Pilot, repeatability and generalization

Start with an agreed small pilot covering the highest-priority failure families, at least one constrained-quality case, one ambiguous case, one physical-cause case and one already-good control. Use the pilot to refine measurement procedures and estimate variance before setting the final acceptance sample size and thresholds.

Separate two kinds of variation:

- **Agent variation:** repeat the same case in fresh conversations with fixed evidence and versioned instructions. Score every recommendation, not just the best one. Where physical testing is limited, select plans by a rule fixed in advance, not by choosing the most promising answer after review.
- **Printing variation:** repeat a frozen recommendation and the unchanged baseline under comparable conditions. Three repeats per path can be an initial pilot check where practical; they do not prove reliability or replace a sample-size decision based on variance and the required effect.

Several prints from the same model/failure episode are repeated measurements of that case, not several independent demonstrations of generalization. Report results by problem family, geometry, printer/material combination and evidence completeness. Include held-out real parts and less familiar materials/setups within the product's supported range. Do not extrapolate results from one printer or a single coupon to all users.

For comparing agents/models, use the same cases, evidence, clarification rules and intervention budget. Hold the application instructions/tools constant for a model comparison. Record provider/model version, relevant generation settings where available and the full conversation. Show case-level results and uncertainty; do not rank agents from one successful demonstration or pooled scores dominated by easy cases.

## 7. Acceptance and reporting

Before the held-out acceptance run, the product/process owners should agree on:

- Required quality and functional limits for each case family, and what counts as a meaningful improvement.
- Target first-attempt and within-budget recovery rates, acceptable regression rate and limits on time/material/user effort.
- The printer/material/geometry populations those targets cover, and the evidence needed to make a claim about each.

These targets are to be set from requirements and the pilot, not invented from the existing software test count. Avoid a single aggregate pass rate that hides poor performance on a critical defect family.

Report, for each episode: initial defect and independently known/unknown cause; evidence provided; assistant questions and exact recommendations; what was actually applied; each attempted print and measurement; constraint violations; outcome and tradeoffs; baseline/expert comparison; and effort/cost. Distinguish **improved**, **recovered**, **useful next diagnostic step**, **no meaningful change**, **worsened**, **non-actionable** and **not evaluated**, with reasons. A useful next step is not recorded as a recovered print until recovery is demonstrated.

The final deliverable should answer: **Which kinds of printing problems does the assistant help with, how often does it help on the tested population, what does that improvement cost, where does it make things worse, and when should it ask for more evidence or defer to a physical check?**

## Method references and implementation prerequisites

Recording process settings and measuring defined artifact features follows the general approach described in the [NIST Additive Manufacturing Test Artifact](https://www.nist.gov/el/intelligent-systems-division-73500/production-systems-group/nist-additive-manufacturing-test). The specific coupons, quality rubrics and acceptance criteria above are a proposed product evaluation, not a claim of NIST or standards compliance.

Manufacturer troubleshooting documentation is useful when constructing realistic differential cases. For example, Prusa's [layer-shifting guidance](https://help.prusa3d.com/article/layer-shifting_2020) includes mechanical checks, illustrating why some apparent print-quality failures require more than slicer tuning. Its printer-specific remedies should not be copied as universal settings for other machines.

Before a physical run, confirm the application can inspect the intended project, apply the selected supported edits and expose current slice evidence. Record any integration blocker separately. Retain the existing software regression suite as that prerequisite; it is not the main process-effectiveness benchmark described here.
