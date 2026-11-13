#pragma once

#include <glib-object.h>

// GDBusInterfaceInfo has a member named `signals`, which Qt replaces with a
// keyword macro, so the gcr headers only parse with the macro put aside.
#pragma push_macro("signals")
#undef signals
#include <gcr/gcr.h>
#pragma pop_macro("signals")

// The GObject type below is named after GObject conventions on purpose.
// NOLINTBEGIN(readability-identifier-naming)

G_BEGIN_DECLS

#define QS_TYPE_GCR_PROMPT (qs_gcr_prompt_get_type())
G_DECLARE_FINAL_TYPE(QsGcrPrompt, qs_gcr_prompt, QS, GCR_PROMPT, GObject)

G_END_DECLS

// NOLINTEND(readability-identifier-naming)

namespace qs::service::keyringprompter {

//! GType of the GcrPrompt implementation handed to the system prompter.
//!
//! gcr 4 keeps the org.gnome.keyring.SystemPrompter D-Bus service, the
//! GcrPrompt interface and the sx-aes-1 secret exchange inside the library and
//! only dropped the gcr-prompter 3.x executable. Implementing the five
//! GcrPrompt virtual methods is therefore enough to serve oo7's unlock
//! requests; the wire protocol stays libgcr's business.
GType promptType();

//! Complete the prompt with the password the user typed.
void completePassword(GcrPrompt* prompt, const char* password);

//! Complete the prompt with the confirmation the user gave.
void completeConfirm(GcrPrompt* prompt, GcrPromptReply reply);

//! Dismiss the prompt as if the user had cancelled it.
void cancelPrompt(GcrPrompt* prompt);

} // namespace qs::service::keyringprompter
