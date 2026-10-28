# Snorca AI assistant product guide

Usage, current features and future possibilities. Updated 28 October 2026.

## The product idea

Snorca's AI assistant helps people turn a printing goal into changes they can review in the slicer. Instead of knowing which of hundreds of settings to search for, a user can describe what they want: clearer lettering, easier support removal, a stronger part, a faster prototype, or help understanding a failed print.

The assistant can inspect the project, explain relevant choices and make supported changes directly. Models and settings update in a live preview, giving the user a chance to inspect the result before keeping it.

The aim is to reduce the knowledge needed to get started while keeping experienced users in control. Defaults remain a useful starting point. The assistant is most valuable when a particular model, problem or goal calls for a deliberate adjustment.

## Who it helps

**New users** can ask for an outcome without already knowing slicer terminology. They can learn what a setting does while seeing its effect on their own project.

**Regular makers** can use conversation for familiar but repetitive work: resizing parts, adding labels, assigning colors and adjusting print settings.

**Experienced users** can ask for focused changes, discuss tradeoffs and inspect the assistant's actions. The normal slicer controls remain available for precise adjustments.

## Getting started

1. Open a project, or begin with an empty plate, and choose **Edit > AI assistant**.
2. Open the assistant's **Settings** and connect an AI service. OpenRouter is the default; Anthropic, official DeepSeek, and BigModel are also available. DeepSeek starts with `deepseek-flash`. BigModel offers standard API and Coding Plan connections; choose the one your account is eligible to use. Enter the corresponding API key and choose a model that supports the assistant's actions and image input when needed.
3. Describe the task. Include the object, desired result and anything that must stay unchanged.
4. Add a photo or screenshot with **+ Images** if it would help explain the problem. PNG and JPEG are supported, with up to four images per message.
5. Watch the changes appear in the slicer. The assistant can work through several supported steps in one request. Expand **Activity** to see what it inspected or changed.
6. Choose **Apply changes** to keep the complete preview, or **Dismiss** to remove it. Slice again after edits and review the new print preview before printing.

You do not normally need to select an object first. The assistant can inspect the objects on the active plate and identify a target from your description. If several objects could match, it may ask which one you mean. Naming objects clearly makes these conversations easier.

The connected AI service charges for usage. Messages, attached images and relevant project information are sent to that service. The provider, model and connection key survive application restart. Keys are saved separately for each provider in the operating system credential store; clear the key in Settings and Save to remove it. Conversations remain in memory for the current application session.

## What it can help with today

### Choose settings for a specific goal

The assistant can inspect current settings and help adjust walls, infill, supports, speed, cooling, temperature and other printing options. It can explain why a change might help and which tradeoffs it introduces.

> “The small lettering is disappearing. Check whether Arachne would help, and explain any other changes you suggest.”

A request such as “make this stronger” benefits from context: how the part will be loaded, which surfaces matter and how much extra print time is acceptable. Stronger, faster and cleaner are often competing goals; the assistant can help make those choices explicit.

### Investigate a failed or disappointing print

A user can describe a symptom, attach a photo and point to the approximate height where the failure began. The assistant can compare that information with the current settings and an existing slice, then suggest possible causes and supported changes.

> “This figure starts making spaghetti around 35 mm. Here is a photo. Check the supports and what happens at that height.”

The assistant can inspect a completed slice that is already available. If it needs a fresh slice or the saved preview needs to be opened, it will ask. A photo and a slice can narrow down a problem, but they cannot establish every physical cause, such as a loose component or wet filament.

### Modify models and add text

The assistant can create basic shapes, move and resize objects, rotate or mirror them, duplicate parts, make cuts, combine or subtract shapes, and simplify supported meshes. New standalone shapes appear centered on the active plate and resting on the bed.

It can also add raised or engraved lettering to suitable flat surfaces. The lettering remains editable with the slicer's normal text controls.

> “Create a cube, make it 120 mm wide, and add ‘made with Snorca’ centered on the front face.”

This is useful for labels, personalized objects and simple preparation tasks. Precise curved lettering, complex sculpting and some other edits still need the normal interactive tools. The assistant can open those tools and explain what remains to be done.

### Assign colors and create mixes

The assistant can assign loaded filaments to objects or parts, color supported surface regions, create mixing recipes and help approximate a requested color. Mixes use compatible filaments already available in the project.

> “Use the loaded red and blue filaments to make a purple mix, then assign it to this cube.”

Mixing patterns and gradients are also available within the slicer's supported mixing features. The preview is an approximation of appearance; the printed result depends on the materials, printer and mixing approach. The assistant does not make an incompatible printer capable of color mixing.

### Explain the existing print preview

The assistant can read information from a completed slice, including warnings, estimated duration, material use and printing behavior near a specified height. This gives the conversation context beyond the settings alone.

> “Why is this print estimated to take so long? Use the current slice to explain where the time goes before changing anything.”

The user still starts slicing and printing. After a change, the previous slice no longer describes the new project, so another slice is needed before reviewing updated estimates.

### Complete several related steps

A request can combine model editing, lettering, colors and supported settings changes. The assistant can work through dependent steps and show one complete preview for review.

> “Resize the box to 100 mm wide, add its name on the lid, and increase the wall count to three.”

The intent is to review the overall result without approving each individual operation. If part of the request requires a different settings scope or manual work, the assistant should explain that remaining step.

## Keeping control of changes

The live preview makes the assistant's work visible while it happens:

- **Apply changes** keeps everything in the current preview. It does not perform the same edits a second time.
- **Dismiss** restores the models, colors and settings from before that preview.
- **Revert changes** can undo an accepted preview while its earlier state is still available and later manual changes have not conflicted with it.
- **Stop** interrupts the conversation's ongoing work and leaves completed changes available for review.

The preview is already part of the visible project. Saving or exporting before Apply includes those visible changes. To remove them, use Dismiss before saving. Starting a new chat dismisses a pending preview; accepted changes remain.

The assistant currently changes one settings scope at a time. Choose it from the dropdown beside the chat input:

| Choice | Where settings changes apply |
| --- | --- |
| Object | The particular object being discussed. |
| Global process | Global print settings used by objects in the project. |
| Global filament | The currently edited filament profile. |
| Global printer | The current printer profile. |

Global scopes work even on an empty plate or with complex objects. The assistant can inspect information outside that choice, but you must switch the dropdown before it can edit a different scope. Individual object settings can also take precedence over global settings. Changes to profiles are not automatically saved permanently to those profiles.

## Asking useful questions

A good request states the desired outcome and the constraints that matter. You do not need to prescribe the exact settings.

| Broad request | More useful request |
| --- | --- |
| “Make it better.” | “Improve the small details on the front. The back is hidden, and I can accept a longer print.” |
| “Make it faster.” | “This is a fit-check prototype. Reduce print time, but keep the outer dimensions unchanged.” |
| “Fix the failure.” | “The supports detach near 35 mm. Here is a photo, and the current project is already sliced.” |
| “Add text.” | “Add HELLO centered on the top face, 5 mm tall and 0.6 mm raised.” |
| “Change the color.” | “Make the lid approximately lavender using the compatible filaments already loaded.” |

You can also ask for an explanation before making changes: “Compare the options first,” “Tell me what you would change,” or “Explain why that setting matters.” Follow-up messages can refine an existing preview, such as “Keep the new dimensions, but make the lettering smaller.”

## Current boundaries

The assistant's knowledge of the project includes its objects, settings, colors and available slice information. It does not automatically see the viewport as a picture. Attach a screenshot when the appearance itself matters.

Some complex projects still need manual settings edits, especially those using variable layer heights, detailed settings modifiers or several copies of an object. The assistant can inspect information and explain the limitation, but not every normal slicer operation is fully automated. Opening an interactive editor still leaves work for the user.

The assistant currently supports filament printing. It does not automatically run comparison slices, submit print jobs, watch a printer camera or verify that a physical print succeeded. Recommendations remain something to review in the slicer and evaluate in the real print.

## What it could become

The following are possible product directions, not features available in the current version or a committed roadmap.

### Compare outcomes before choosing

A future assistant could prepare several alternatives, slice them and compare the results: a fast prototype, a balanced print and a version focused on surface quality. The user could choose using estimated time, material use and visible differences instead of guessing how a setting might affect the result.

For example: “Show me three ways to get this under four hours, and explain what I would give up.”

### Give attention to specific regions

With better visual and geometric understanding, a user could point to an area and say, “Preserve the lettering here, but simplify the hidden underside.” The assistant could identify relevant local adjustments and show exactly where they apply, wherever the slicer supports regional control.

This would bring the interaction closer to describing the finished object, rather than selecting settings for the entire model.

### Check a project before printing

A future preflight review could flag likely problems such as unsupported regions, fragile details, awkward seams or costly color transitions. It could explain which findings matter most and offer a small set of changes for review.

The value would be earlier, clearer feedback on likely problems. Such a review would still need to distinguish a suspected risk from a confirmed defect.

### Learn from completed prints

With a user-approved record of outcomes, the assistant could remember which changes worked for a particular printer, material and type of model. A failed print followed by a successful retry could become useful context for the next project.

This could support a more practical conversation: “Use the support approach that worked on my last miniature.” Persistent preferences and print-outcome history are not available today.

### Guide calibration and diagnosis

The assistant could guide a user through a sequence of calibration prints, compare photographs and observations, and help decide which variable to investigate next. Connecting recommendations to actual results could make troubleshooting more systematic and reduce repeated trial and error.

Printer monitoring could eventually add observations during a print, provided users explicitly choose that connection and retain control over any intervention.

### Make common tasks repeatable

Users could save reusable instructions such as “prepare a fast fit-check prototype,” “label every enclosure lid,” or “use my miniature-printing preferences.” The assistant could apply those preferences to a new project, explain any exceptions and present a combined preview.

The broader opportunity is a conversational printing workflow: describe the outcome, inspect the proposed result, refine it, and learn from what actually prints. The current assistant establishes that workflow through project-aware conversation and reversible changes.
