#include "keyringprompter.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <qlogging.h>
#include <qloggingcategory.h>

#include "../../core/logcat.hpp"

namespace {
QS_LOGGING_CATEGORY(logKeyringPrompter, "quickshell.service.keyringprompter", QtWarningMsg);

//! The only implementation of the D-Bus name oo7 asks for the unlock dialog.
constexpr auto SYSTEM_PROMPTER_NAME = "org.gnome.keyring.SystemPrompter";

//! gcr-prompter's own QUIT_TIMEOUT equivalent: a prompt nobody answers must not
//! block the requesting application forever.
constexpr int PROMPT_TIMEOUT_MS = 60000;
} // namespace

namespace qs::service::keyringprompter {

namespace {
KeyringPrompter* prompterInstance = nullptr;

QString takeString(gchar* value) {
	QString result = value != nullptr ? QString::fromUtf8(value) : QString();
	g_free(value);
	return result;
}
} // namespace

KeyringPrompter::KeyringPrompter(QObject* parent): QObject(parent) {
	this->mTimeout.setSingleShot(true);
	QObject::connect(&this->mTimeout, &QTimer::timeout, this, [this]() {
		qCWarning(logKeyringPrompter) << "no answer for the active keyring prompt after"
		                              << PROMPT_TIMEOUT_MS << "ms, cancelling";
		this->cancel();
	});
}

KeyringPrompter::~KeyringPrompter() {
	if (prompterInstance == this) prompterInstance = nullptr;

	if (this->mPrompt != nullptr) {
		g_object_remove_weak_pointer(
		    G_OBJECT(this->mPrompt),
		    reinterpret_cast<gpointer*>(&this->mPrompt)
		);
		this->mPrompt = nullptr;
	}

	if (this->mOwnerId != 0) {
		g_bus_unown_name(this->mOwnerId);
		this->mOwnerId = 0;
	}

	if (this->mSystemPrompter != nullptr) {
		if (this->mRegistered) gcr_system_prompter_unregister(this->mSystemPrompter, FALSE);
		g_clear_object(&this->mSystemPrompter);
	}

	g_clear_object(&this->mConnection);
}

void KeyringPrompter::componentComplete() {
	if (prompterInstance != nullptr) {
		qCWarning(logKeyringPrompter) << "only one KeyringPrompter may exist, ignoring the new one";
		return;
	}

	GError* error = nullptr;
	this->mConnection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
	if (this->mConnection == nullptr) {
		qCWarning(
		    logKeyringPrompter
		) << "cannot connect to the session bus, keyring prompts will not be served:"
		  << (error != nullptr ? error->message : "unknown error");
		g_clear_error(&error);
		return;
	}

	// The getter of the same name is a QString property; the GType comes from
	// the namespace-level promptType().
	this->mSystemPrompter = gcr_system_prompter_new(
	    GCR_SYSTEM_PROMPTER_SINGLE,
	    qs::service::keyringprompter::promptType()
	);
	prompterInstance = this;

	this->mOwnerId = g_bus_own_name_on_connection(
	    this->mConnection,
	    SYSTEM_PROMPTER_NAME,
	    G_BUS_NAME_OWNER_FLAGS_NONE,
	    &KeyringPrompter::onNameAcquired,
	    &KeyringPrompter::onNameLost,
	    this,
	    nullptr
	);
}

void KeyringPrompter::onNameAcquired(
    GDBusConnection* connection,
    const gchar* /*name*/,
    gpointer userData
) {
	auto* self = static_cast<KeyringPrompter*>(userData);
	if (self != prompterInstance) return;

	gcr_system_prompter_register(self->mSystemPrompter, connection);
	self->mRegistered = true;
	qCDebug(logKeyringPrompter) << "registered the system prompter";
	emit self->isRegisteredChanged();
}

void KeyringPrompter::onNameLost(
    GDBusConnection* /*connection*/,
    const gchar* name,
    gpointer userData
) {
	auto* self = static_cast<KeyringPrompter*>(userData);
	if (self != prompterInstance) return;

	self->mRegistered = false;
	qCWarning(logKeyringPrompter) << "cannot own" << name
	                              << "- gcr 3's gcr-prompter is probably still installed";
	emit self->isRegisteredChanged();
}

void KeyringPrompter::handlePromptRequest(GcrPrompt* prompt, bool confirm) {
	if (this->mPrompt != prompt) {
		if (this->mPrompt != nullptr) {
			g_object_remove_weak_pointer(
			    G_OBJECT(this->mPrompt),
			    reinterpret_cast<gpointer*>(&this->mPrompt)
			);
		}
		this->mPrompt = prompt;
		g_object_add_weak_pointer(G_OBJECT(prompt), reinterpret_cast<gpointer*>(&this->mPrompt));
	}

	this->mConfirm = confirm;
	this->mMessage = takeString(gcr_prompt_get_message(prompt));
	this->mDescription = takeString(gcr_prompt_get_description(prompt));
	this->mWarning = takeString(gcr_prompt_get_warning(prompt));
	this->mPasswordNew = gcr_prompt_get_password_new(prompt) != FALSE;

	emit this->promptChanged();
	this->setActive(true);
	this->mTimeout.start(PROMPT_TIMEOUT_MS);
}

void KeyringPrompter::handlePromptClosed() {
	if (this->mPrompt != nullptr) {
		g_object_remove_weak_pointer(
		    G_OBJECT(this->mPrompt),
		    reinterpret_cast<gpointer*>(&this->mPrompt)
		);
		this->mPrompt = nullptr;
	}

	this->setActive(false);
}

void KeyringPrompter::respond(const QString& password) {
	if (!this->mActive || this->mConfirm || this->mPrompt == nullptr) {
		qCWarning(logKeyringPrompter) << "ignoring a password reply that no prompt asked for";
		return;
	}

	auto utf8 = password.toUtf8();
	completePassword(this->mPrompt, utf8.constData());
	this->finishActivePrompt();
}

void KeyringPrompter::confirm() {
	if (!this->mActive || !this->mConfirm || this->mPrompt == nullptr) {
		qCWarning(logKeyringPrompter) << "ignoring a confirmation that no prompt asked for";
		return;
	}

	completeConfirm(this->mPrompt, GCR_PROMPT_REPLY_CONTINUE);
	this->finishActivePrompt();
}

void KeyringPrompter::cancel() {
	if (this->mPrompt != nullptr && this->mActive) {
		if (this->mConfirm) completeConfirm(this->mPrompt, GCR_PROMPT_REPLY_CANCEL);
		else cancelPrompt(this->mPrompt);
	}

	this->finishActivePrompt();
}

void KeyringPrompter::finishActivePrompt() {
	this->mTimeout.stop();
	this->setActive(false);
}

void KeyringPrompter::setActive(bool active) {
	if (this->mActive == active) return;

	this->mActive = active;
	emit this->isActiveChanged();
}

KeyringPrompter* activePrompter() { return prompterInstance; }

} // namespace qs::service::keyringprompter
