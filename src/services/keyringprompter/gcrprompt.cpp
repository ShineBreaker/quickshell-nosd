#include "gcrprompt.hpp"

#include <gio/gio.h>
#include <glib.h>

#include "keyringprompter.hpp"

// This is GObject code, named after GObject conventions on purpose.
// NOLINTBEGIN(readability-identifier-naming,misc-use-anonymous-namespace)

using qs::service::keyringprompter::KeyringPrompter;

struct _QsGcrPrompt {
	GObject parent_instance;

	//! Every GcrPrompt property of the interface, echoed onto this class so the
	//! library can g_object_set_property() before each PerformPrompt round. The
	//! prompt object is reused across retries, hence per-instance storage.
	GHashTable* properties;

	//! The password or confirmation request in flight, NULL when idle.
	GTask* task;

	//! The password handed out by the last successful prompt.
	gchar* secret;

	//! Whether the pending task is a confirmation rather than a password.
	gboolean confirm;
};

static void qs_gcr_prompt_interface_init(GcrPromptInterface* iface);

G_DEFINE_TYPE_WITH_CODE(
    QsGcrPrompt,
    qs_gcr_prompt,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(GCR_TYPE_PROMPT, qs_gcr_prompt_interface_init)
)

static void qs_gcr_prompt_value_free(gpointer data) {
	auto* value = static_cast<GValue*>(data);
	g_value_unset(value);
	g_free(value);
}

//! Props of the GcrPrompt interface. Takes the interface vtable rather than
//! the GType because that is what g_object_interface_list_properties() expects.
static GParamSpec** qs_gcr_prompt_interface_properties(guint* count) {
	auto* iface = static_cast<GcrPromptInterface*>(g_type_default_interface_ref(GCR_TYPE_PROMPT));
	auto** specs = g_object_interface_list_properties(iface, count);
	g_type_default_interface_unref(iface);
	return specs;
}

static GValue* qs_gcr_prompt_property(QsGcrPrompt* self, const char* name) {
	return static_cast<GValue*>(g_hash_table_lookup(self->properties, name));
}

//! Fail the pending request with G_IO_ERROR_CANCELLED, which the prompter
//! reports to the caller as "no" (keyring not unlocked) instead of hanging.
static void qs_gcr_prompt_cancel_task(QsGcrPrompt* self) {
	if (self->task == nullptr) return;

	g_task_return_new_error(
	    self->task,
	    G_IO_ERROR,
	    G_IO_ERROR_CANCELLED,
	    "The keyring prompt was closed"
	);
	g_clear_object(&self->task);
}

static void qs_gcr_prompt_init(QsGcrPrompt* self) {
	self->properties =
	    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, qs_gcr_prompt_value_free);
	self->task = nullptr;
	self->secret = nullptr;
	self->confirm = FALSE;

	guint count = 0;
	auto** specs = qs_gcr_prompt_interface_properties(&count);
	for (guint i = 0; i < count; i++) {
		auto* value = g_new0(GValue, 1);
		g_value_init(value, specs[i]->value_type);
		g_param_value_set_default(specs[i], value);
		g_hash_table_insert(self->properties, g_strdup(specs[i]->name), value);
	}
	g_free(specs);
}

static void qs_gcr_prompt_dispose(GObject* object) {
	qs_gcr_prompt_cancel_task(QS_GCR_PROMPT(object));
	G_OBJECT_CLASS(qs_gcr_prompt_parent_class)->dispose(object);
}

static void qs_gcr_prompt_finalize(GObject* object) {
	auto* self = QS_GCR_PROMPT(object);

	g_clear_pointer(&self->properties, g_hash_table_unref);
	g_clear_pointer(&self->secret, g_free);

	G_OBJECT_CLASS(qs_gcr_prompt_parent_class)->finalize(object);
}

static void
qs_gcr_prompt_set_property(GObject* object, guint /*id*/, const GValue* value, GParamSpec* pspec) {
	auto* stored = qs_gcr_prompt_property(QS_GCR_PROMPT(object), pspec->name);
	if (stored != nullptr) g_value_copy(value, stored);
}

static void
qs_gcr_prompt_get_property(GObject* object, guint /*id*/, GValue* value, GParamSpec* pspec) {
	auto* stored = qs_gcr_prompt_property(QS_GCR_PROMPT(object), pspec->name);
	if (stored != nullptr) g_value_copy(stored, value);
}

static void qs_gcr_prompt_class_init(QsGcrPromptClass* klass) {
	auto* objectClass = G_OBJECT_CLASS(klass);
	objectClass->set_property = qs_gcr_prompt_set_property;
	objectClass->get_property = qs_gcr_prompt_get_property;
	objectClass->dispose = qs_gcr_prompt_dispose;
	objectClass->finalize = qs_gcr_prompt_finalize;

	// The GcrPrompt interface declares the properties but stores nothing, so a
	// GcrPrompt implementation has to expose them on its own class. Installing
	// the interface specs is not allowed (their owner_type is already set), so
	// each one is overridden instead.
	guint count = 0;
	auto** specs = qs_gcr_prompt_interface_properties(&count);
	for (guint i = 0; i < count; i++) {
		g_object_class_override_property(objectClass, i + 1, specs[i]->name);
	}
	g_free(specs);
}

static void qs_gcr_prompt_password_async(
    GcrPrompt* prompt,
    GCancellable* cancellable,
    GAsyncReadyCallback callback,
    gpointer userData
) {
	auto* self = QS_GCR_PROMPT(prompt);

	qs_gcr_prompt_cancel_task(self);
	self->task = g_task_new(prompt, cancellable, callback, userData);
	self->confirm = FALSE;

	auto* bridge = qs::service::keyringprompter::activePrompter();
	if (bridge != nullptr) {
		bridge->handlePromptRequest(prompt, FALSE);
	} else {
		// No UI consumer: never leave the caller waiting for a dialog that will
		// not open.
		qs_gcr_prompt_cancel_task(self);
	}
}

static const gchar*
qs_gcr_prompt_password_finish(GcrPrompt* prompt, GAsyncResult* result, GError** error) {
	auto* self = QS_GCR_PROMPT(prompt);

	auto* secret = static_cast<gchar*>(g_task_propagate_pointer(G_TASK(result), error));
	if (secret == nullptr) return nullptr;

	// The prompter encrypts the returned pointer into the secret exchange
	// immediately but does not free it, so the prompt keeps owning it.
	g_free(self->secret);
	self->secret = secret;

	return self->secret;
}

static void qs_gcr_prompt_confirm_async(
    GcrPrompt* prompt,
    GCancellable* cancellable,
    GAsyncReadyCallback callback,
    gpointer userData
) {
	auto* self = QS_GCR_PROMPT(prompt);

	qs_gcr_prompt_cancel_task(self);
	self->task = g_task_new(prompt, cancellable, callback, userData);
	self->confirm = TRUE;

	auto* bridge = qs::service::keyringprompter::activePrompter();
	if (bridge != nullptr) {
		bridge->handlePromptRequest(prompt, TRUE);
	} else {
		qs_gcr_prompt_cancel_task(self);
	}
}

static GcrPromptReply
qs_gcr_prompt_confirm_finish(GcrPrompt* /*prompt*/, GAsyncResult* result, GError** error) {
	auto reply = g_task_propagate_int(G_TASK(result), error);
	if (reply < 0) return GCR_PROMPT_REPLY_CANCEL;
	return static_cast<GcrPromptReply>(reply);
}

static void qs_gcr_prompt_close(GcrPrompt* prompt) {
	qs_gcr_prompt_cancel_task(QS_GCR_PROMPT(prompt));

	auto* bridge = qs::service::keyringprompter::activePrompter();
	if (bridge != nullptr) bridge->handlePromptClosed();
}

static void qs_gcr_prompt_interface_init(GcrPromptInterface* iface) {
	iface->prompt_password_async = qs_gcr_prompt_password_async;
	iface->prompt_password_finish = qs_gcr_prompt_password_finish;
	iface->prompt_confirm_async = qs_gcr_prompt_confirm_async;
	iface->prompt_confirm_finish = qs_gcr_prompt_confirm_finish;
	iface->prompt_close = qs_gcr_prompt_close;
}

namespace qs::service::keyringprompter {
GType promptType() { return QS_TYPE_GCR_PROMPT; }

void completePassword(GcrPrompt* prompt, const char* password) {
	auto* self = QS_GCR_PROMPT(prompt);
	if (self->task == nullptr) return;

	g_task_return_pointer(self->task, g_strdup(password), g_free);
	g_clear_object(&self->task);
}

void completeConfirm(GcrPrompt* prompt, GcrPromptReply reply) {
	auto* self = QS_GCR_PROMPT(prompt);
	if (self->task == nullptr) return;

	g_task_return_int(self->task, reply);
	g_clear_object(&self->task);
}

void cancelPrompt(GcrPrompt* prompt) { qs_gcr_prompt_cancel_task(QS_GCR_PROMPT(prompt)); }
} // namespace qs::service::keyringprompter

// NOLINTEND(readability-identifier-naming,misc-use-anonymous-namespace)
