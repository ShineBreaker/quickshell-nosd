name = "Quickshell.Services.Keyring"
description = "Keyring Prompter"
headers = [
	"gcrprompt.hpp",
	"keyringprompter.hpp",
]
-----
## Serving keyring unlocks

Applications that use the Secret Service ask the desktop to unlock their
keyring through the `org.gnome.keyring.SystemPrompter` D-Bus name. The only
implementation of that service in current distributions is gcr 3's
`gcr-prompter`, and gcr 4 no longer ships it even though the prompt interface
and the D-Bus service stayed in the library.

This service rebuilds the missing dialog side on top of libgcr: it owns the
bus name and, for every `PerformPrompt` call, opens the @@KeyringPrompter
properties for the shell to display. The password is handed back to the
library, which encrypts it with the `sx-aes-1` exchange; the shell never sees
the wire protocol.

## Using the prompter

Instantiating a @@KeyringPrompter is enough — the object registers on the
session bus in `componentComplete()` and @@KeyringPrompter.isRegistered reports
whether the name was acquired. When another implementation holds the name,
registration fails and the shell stays out of the way.

An active prompt is announced by @@KeyringPrompter.isActive. The prompt text
lives in @@KeyringPrompter.message / @@KeyringPrompter.description and, from
the second attempt on, @@KeyringPrompter.warning; the prompt object is reused
across retries, so @@KeyringPrompter.promptChanged fires again for every round
and the UI must refresh its fields rather than keep the first values.

Answers are sent with @@KeyringPrompter.respond() for @@KeyringPrompter.promptType
`password` and @@KeyringPrompter.confirm() / @@KeyringPrompter.cancel() for
`confirm`. A prompt that nobody answers is cancelled after 60 seconds so that
the asking application gets an error instead of blocking forever, and the same
happens when no @@KeyringPrompter exists at all.
