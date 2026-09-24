#pragma once

// Service management for the daemon.
// With init/flashlightd.rc installed, init owns the daemon and these drive it through
// ctl.start/ctl.stop. Without it, `start` detaches a supervisor that runs `flashlightd daemon`
// and restarts it whenever it exits (like a non-oneshot init service); `stop` ends both.
namespace flashlightd::service {

int start();
int stop();
int restart();

// Prints service state and pids; returns true when the service is running.
bool printState();

}  // namespace flashlightd::service
