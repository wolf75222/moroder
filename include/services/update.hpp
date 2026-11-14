/* UPDATES AND HOUSEKEEPING COMMANDS
- At launch (CHECK_FOR_UPDATES, at most once an hour, 1.5 s at most, before the
  interface starts): if a newer release is out, ask "Update now? [Y/n]"; yes
  runs the installer and restarts into the new version; no is remembered for
  that version (`moroder update` still installs it)
- One request to GitHub's public API, nothing about the user is sent
- `moroder update`, `moroder doctor`, `moroder uninstall`, `moroder --version`
*/
#pragma once

namespace services::update {

// Returns true when it updated and the caller should restart (execvp).
bool offerAtLaunch();

int runUpdate();        // moroder update
int doctor();           // moroder doctor
int uninstall();        // moroder uninstall
const char* version();

}
