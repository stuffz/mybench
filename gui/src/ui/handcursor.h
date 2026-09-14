#pragma once
// Item views and tab bars paint their rows and tabs instead of building a
// widget for each one, so AppStyle::polish never sees them and there is nothing
// to hand a cursor to. Setting it on the view itself would claim the empty
// space past the last row as well, which opens nothing.
//
// These watch the pointer and set the hand only while it is over something
// clickable. Use them on navigational lists, not on grids of data.
class QAbstractItemView;
class QTabBar;

void handCursorOnRows(QAbstractItemView *view);
void handCursorOnTabs(QTabBar *bar);
