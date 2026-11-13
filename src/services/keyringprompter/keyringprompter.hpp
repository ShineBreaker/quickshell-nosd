#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qqmlparserstatus.h>
#include <qtimer.h>

#include "gcrprompt.hpp"

namespace qs::service::keyringprompter {

//! Password dialog backend for the oo7 Secret Service keyring.
//!
//! Owns `org.gnome.keyring.SystemPrompter` through libgcr's system prompter and
//! forwards the prompts it receives to the shell as Qt properties. The library
//! speaks the D-Bus protocol and the sx-aes-1 secret exchange; this class only
//! carries the user's answer back into the prompt, so the QML side never sees
//! the exchange.
class KeyringPrompter
    : public QObject
    , public QQmlParserStatus {
	Q_OBJECT;
	QML_ELEMENT;
	Q_INTERFACES(QQmlParserStatus);
	Q_DISABLE_COPY_MOVE(KeyringPrompter);

	//! Whether the prompter owns the SystemPrompter bus name. False when another
	//! implementation (gcr 3's gcr-prompter) owns it or no session bus is present.
	Q_PROPERTY(bool isRegistered READ isRegistered NOTIFY isRegisteredChanged);

	//! Whether a prompt is waiting for user input.
	Q_PROPERTY(bool isActive READ isActive NOTIFY isActiveChanged);

	//! Either "password" or "confirm".
	Q_PROPERTY(QString promptType READ promptType NOTIFY promptChanged);

	//! The question to show, always set by the caller.
	Q_PROPERTY(QString message READ message NOTIFY promptChanged);

	//! Supporting text, empty when the caller did not provide any.
	Q_PROPERTY(QString description READ description NOTIFY promptChanged);

	//! Why the previous attempt failed, empty on the first round.
	Q_PROPERTY(QString warning READ warning NOTIFY promptChanged);

	//! Whether the password is being created instead of unlocked.
	Q_PROPERTY(bool passwordNew READ passwordNew NOTIFY promptChanged);

public:
	explicit KeyringPrompter(QObject* parent = nullptr);
	~KeyringPrompter() override;

	void classBegin() override {};
	void componentComplete() override;

	[[nodiscard]] bool isRegistered() const { return this->mRegistered; }
	[[nodiscard]] bool isActive() const { return this->mActive; }
	[[nodiscard]] QString promptType() const { return this->mConfirm ? "confirm" : "password"; }
	[[nodiscard]] QString message() const { return this->mMessage; }
	[[nodiscard]] QString description() const { return this->mDescription; }
	[[nodiscard]] QString warning() const { return this->mWarning; }
	[[nodiscard]] bool passwordNew() const { return this->mPasswordNew; }

	//! Submit the typed password. Ignored when no password prompt is active.
	Q_INVOKABLE void respond(const QString& password);

	//! Accept the active confirmation prompt. Ignored for password prompts.
	Q_INVOKABLE void confirm();

	//! Dismiss the active prompt; the caller sees the secret as not entered.
	Q_INVOKABLE void cancel();

	// Implementation detail for the GcrPrompt bridge, not callable from QML.
	void handlePromptRequest(GcrPrompt* prompt, bool confirm);
	void handlePromptClosed();

signals:
	void isRegisteredChanged();
	void isActiveChanged();
	void promptChanged();

private:
	void finishActivePrompt();
	void setActive(bool active);

	static void onNameAcquired(GDBusConnection* connection, const gchar* name, gpointer userData);
	static void onNameLost(GDBusConnection* connection, const gchar* name, gpointer userData);

	GDBusConnection* mConnection = nullptr;
	GcrSystemPrompter* mSystemPrompter = nullptr;
	//! Weak: the library owns the prompt and drops it when prompting stops.
	GcrPrompt* mPrompt = nullptr;
	guint mOwnerId = 0;
	bool mConfirm = false;
	bool mActive = false;
	bool mRegistered = false;
	bool mPasswordNew = false;
	QString mMessage;
	QString mDescription;
	QString mWarning;
	//! Never leave a caller blocked when no UI consumes the signal.
	QTimer mTimeout;
};

//! The single prompter instance, or null before the QML object is complete.
KeyringPrompter* activePrompter();

} // namespace qs::service::keyringprompter
