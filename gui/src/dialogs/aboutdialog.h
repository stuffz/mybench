#pragma once
// The About box: what this build is, and what it is built on.

class QWidget;

// Asks the backend for the build stamps first, so the dialog only appears once
// there is something to put in it.
void showAboutDialog(QWidget *parent);
