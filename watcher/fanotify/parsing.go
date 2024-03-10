package fanotify

import (
	"fmt"

	"golang.org/x/sys/unix"
)

func parseEventMask(mask uint64) (string, error) {
	var events []string
	if mask&unix.FAN_ACCESS != 0 {
		events = append(events, "ACCESS")
	}
	if mask&unix.FAN_MODIFY != 0 {
		events = append(events, "MODIFY")
	}
	if mask&unix.FAN_CLOSE_WRITE != 0 {
		events = append(events, "CLOSE_WRITE")
	}
	if mask&unix.FAN_CLOSE_NOWRITE != 0 {
		events = append(events, "CLOSE_NOWRITE")
	}
	if mask&unix.FAN_OPEN != 0 {
		events = append(events, "OPEN")
	}
	if mask&unix.FAN_Q_OVERFLOW != 0 {
		events = append(events, "Q_OVERFLOW")
	}
	if mask&unix.FAN_OPEN_PERM != 0 {
		events = append(events, "OPEN_PERM")
	}
	if mask&unix.FAN_ACCESS_PERM != 0 {
		events = append(events, "ACCESS_PERM")
	}
	if mask&unix.FAN_ONDIR != 0 {
		events = append(events, "ONDIR")
	}
	if mask&unix.FAN_EVENT_ON_CHILD != 0 {
		events = append(events, "EVENT_ON_CHILD")
	}
	if mask&unix.FAN_CLOSE != 0 {
		// Less specific than CLOSE_WRITE and CLOSE_NOWRITE
		//events = append(events, "CLOSE")
	}
	if mask&unix.FAN_OPEN_EXEC_PERM != 0 {
		events = append(events, "OPEN_EXEC_PERM")
	}
	if mask&unix.FAN_OPEN_EXEC != 0 {
		events = append(events, "OPEN_EXEC")
	}
	if mask&unix.FAN_CREATE != 0 {
		events = append(events, "CREATE")
	}
	if mask&unix.FAN_DELETE != 0 {
		events = append(events, "DELETE")
	}
	if mask&unix.FAN_DELETE_SELF != 0 {
		events = append(events, "DELETE_SELF")
	}
	if mask&unix.FAN_MOVE != 0 {
		// Less specific than MOVED_FROM and MOVED_TO
		//events = append(events, "MOVE")
	}
	if mask&unix.FAN_MOVED_FROM != 0 {
		events = append(events, "MOVED_FROM")
	}
	if mask&unix.FAN_MOVED_TO != 0 {
		events = append(events, "MOVED_TO")
	}

	if mask&unix.FAN_MOVE_SELF != 0 {
		events = append(events, "MOVE_SELF")
	}
	if mask&unix.FAN_ATTRIB != 0 {
		events = append(events, "ATTRIB")
	}
	if len(events) == 0 {
		return "", fmt.Errorf("No events found for mask: %d", mask)
	}
	if len(events) > 1 {
		return "", fmt.Errorf("Multiple events found for mask: %d %v", mask, events)
	}
	return events[0], nil

}

func parseEventMaskSimplified(mask uint64) (string, error) {
	var events []string
	if mask&unix.FAN_CLOSE_WRITE != 0 {
		events = append(events, "close_for_write")
	}
	if mask&unix.FAN_DELETE != 0 {
		events = append(events, "remove")
	}
	if mask&unix.FAN_MOVED_FROM != 0 {
		events = append(events, "move_from")
	}
	if mask&unix.FAN_MOVED_TO != 0 {
		events = append(events, "move_to")
	}
	if len(events) == 0 {
		return "", fmt.Errorf("No events found for mask: %d", mask)
	}
	if len(events) > 1 {
		return "multiple", nil
	}
	return events[0], nil
}
