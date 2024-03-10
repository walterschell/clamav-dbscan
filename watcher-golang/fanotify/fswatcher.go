package fanotify

import (
	"encoding/json"
	"fmt"
	"os"
	"unsafe"

	"github.com/fatih/color"
	"github.com/yookoala/realpath"
	"golang.org/x/sys/unix"
)

type Event struct {
	Dirpath   *string
	Filename  string
	EventMask uint64
}

func (e *Event) Action() string {
	result, err := parseEventMask(e.EventMask)
	if err != nil {
		panic(err)
	}
	return fmt.Sprintf("%v", result)
}

func (e *Event) ActionSimplified() string {
	result, err := parseEventMaskSimplified(e.EventMask)
	if err != nil {
		panic(err)
	}
	return result
}

func (e *Event) Path(missingPathPlaceholder ...string) string {
	if len(missingPathPlaceholder) > 1 {
		panic("Too many arguments")
	}

	dirpath := e.Dirpath
	if dirpath == nil {
		if len(missingPathPlaceholder) > 0 {
			dirpath = &missingPathPlaceholder[0]
		} else {
			panic("Empty dirpath and missingPathPlaceholder no specified")
		}
	}

	return fmt.Sprintf("%s/%s", *dirpath, e.Filename)
}

func (e Event) String() string {
	return fmt.Sprintf("%s: %s", e.Action(), e.Path("!!!MISSING DIRPATH!!!"))
}

func (e *Event) MarshalJSON() ([]byte, error) {
	return json.Marshal(map[string]interface{}{
		"path":      e.Path("!!!MISSING DIRPATH!!!"),
		"operation": e.ActionSimplified(),
	})
}

type FSWatcherSubscription struct {
	ch                 chan []Event
	unsubscribeChannel chan interface{}
}

func (s *FSWatcherSubscription) Close() {
	s.unsubscribeChannel <- removeChannelMessage{ch: s.ch}
}

func (s *FSWatcherSubscription) Channel() <-chan []Event {
	return s.ch
}

type FSWatcherJsonSubscription struct {
	ch                 chan []byte
	unsubscribeChannel chan interface{}
}

func (s *FSWatcherJsonSubscription) Close() {
	s.unsubscribeChannel <- removeJsonChannelMessage{ch: s.ch}
}

func (s *FSWatcherJsonSubscription) Channel() <-chan []byte {
	return s.ch
}

type addChanelMessage struct {
	ch chan []Event
}
type removeChannelMessage struct {
	ch chan []Event
}

type addJsonChanelMessage struct {
	ch chan []byte
}

type removeJsonChannelMessage struct {
	ch chan []byte
}

type FSWatcher struct {
	file              *os.File
	fs_fd             int
	subscribers       map[chan []Event]interface{}
	jsonSubscribers   map[chan []byte]interface{}
	controlChannel    chan interface{}
	comparePath       string
	comparePathPrefix string
	filterResults     bool
}

func NewFSWatcher(path string, onlyMathing bool) (*FSWatcher, error) {

	watchpath, err := realpath.Realpath(path)
	if err != nil {
		return nil, fmt.Errorf("Failed to resolve path %s: %v", path, err)
	}

	fmt.Printf("Watching %s for %s\n", watchpath, path)
	comparePathPrefix := watchpath + "/"
	comparePath := comparePathPrefix[:len(comparePathPrefix)-1]
	// Note: We require FID and DFID to be able to catch
	// file renames and moves, however this means sometimes
	// we can't resolve the parent directory of the file because
	// it has been moved or deleted. This is a limitation of
	// fanotify, file_handles, and open_by_handle_at.
	fd, err := unix.FanotifyInit(unix.FAN_CLASS_NOTIF|
		unix.FAN_REPORT_FID|
		unix.FAN_REPORT_DFID_NAME|
		unix.FAN_UNLIMITED_QUEUE,
		unix.O_RDONLY)
	if err != nil {
		return nil, err
	}
	err = unix.FanotifyMark(fd,
		unix.FAN_MARK_ADD|unix.FAN_MARK_FILESYSTEM,
		unix.FAN_CLOSE_WRITE|unix.FAN_DELETE|unix.FAN_MOVE,
		0,
		path)
	if err != nil {
		unix.Close(fd)
		return nil, err
	}

	fs_fd, err := unix.Open(path, unix.O_RDONLY|unix.O_DIRECTORY, 0)
	if err != nil {
		unix.Close(fd)
		return nil, err
	}

	file := os.NewFile(uintptr(fd), "fanotify")
	controlChannel := make(chan interface{})
	subscribers := make(map[chan []Event]interface{})
	jsonSubscribers := make(map[chan []byte]interface{})
	results := &FSWatcher{
		file:              file,
		fs_fd:             fs_fd,
		controlChannel:    controlChannel,
		subscribers:       subscribers,
		jsonSubscribers:   jsonSubscribers,
		comparePath:       comparePath,
		comparePathPrefix: comparePathPrefix,
		filterResults:     onlyMathing,
	}
	go results.run()
	return results, nil
}

func (w *FSWatcher) AddSubscription() FSWatcherSubscription {
	ch := make(chan []Event, 10)
	w.controlChannel <- addChanelMessage{ch: ch}
	return FSWatcherSubscription{ch: ch, unsubscribeChannel: w.controlChannel}
}

func (w *FSWatcher) AddJsonSubscription() FSWatcherJsonSubscription {
	ch := make(chan []byte, 10)
	w.controlChannel <- addJsonChanelMessage{ch: ch}
	return FSWatcherJsonSubscription{ch: ch, unsubscribeChannel: w.controlChannel}
}

func (w *FSWatcher) run() {

	eventFeed := make(chan []Event)
	go func() {
		for {
			events, err := w.ReadEvents()
			if err != nil {
				fmt.Printf("Error reading events: %s\n", err)
			} else {
				println("Sending events")
				eventFeed <- events
				println("Sent events")
			}
		}
	}()

	for {
		select {
		case msg := <-w.controlChannel:
			switch msg := msg.(type) {
			case addChanelMessage:
				w.subscribers[msg.ch] = nil
			case removeChannelMessage:
				delete(w.subscribers, msg.ch)
			case addJsonChanelMessage:
				w.jsonSubscribers[msg.ch] = nil
			case removeJsonChannelMessage:
				delete(w.jsonSubscribers, msg.ch)
			}
		case events := <-eventFeed:
			if len(events) == 0 {
				continue
			}
			for ch, _ := range w.subscribers {
				select {
				case ch <- events:
				default:
					color.Yellow("Dropping events on channel %v\n", ch)
				}
			}
			if len(w.jsonSubscribers) > 0 {
				serializedEvents, err := json.Marshal(map[string]interface{}{"events": events})
				if err != nil {
					fmt.Printf("Error serializing events: %s\n", err)
				} else {
					for ch, _ := range w.jsonSubscribers {
						select {
						case ch <- serializedEvents:
						default:
							color.Yellow("Dropping events on JSON channel %v\n", ch)
						}
					}
				}
			}
		}
	}
}

func (w *FSWatcher) Close() {
	w.file.Close()
	w.file = nil
	unix.Close(w.fs_fd)
	w.fs_fd = -1
}

const FAN_EVENT_METADATA_LEN = unsafe.Sizeof(unix.FanotifyEventMetadata{})

func FAN_EVENT_OK(metadata_itor unsafe.Pointer, remainingBufferSize int) bool {
	if uintptr(remainingBufferSize) < FAN_EVENT_METADATA_LEN {
		//fmt.Printf("Remaining buffer size %d is less than metadata size %d\n", remainingBufferSize, FAN_EVENT_METADATA_LEN)
		return false
	}
	metadata := (*unix.FanotifyEventMetadata)(metadata_itor)
	if uintptr(metadata.Event_len) < FAN_EVENT_METADATA_LEN {
		//fmt.Printf("Event length %d is less than metadata size %d\n", metadata.Event_len, FAN_EVENT_METADATA_LEN)
		return false
	}
	if uint32(remainingBufferSize) < metadata.Event_len {
		//fmt.Printf("Remaining buffer size %d is less than event length %d\n", remainingBufferSize, metadata.Event_len)
		return false
	}
	return true
}

type fanotify_event_info_header struct {
	info_type uint8
	pad       uint8
	len       uint16
}

type file_handle struct {
	handle_bytes uint32 //TODO: Check 64 bit
	handle_type  int32
}

func NewFileHandle(handle []byte) (unixHandle unix.FileHandle, remaining []byte, err error) {
	//fmt.Printf("NewFileHandle: %d bytes\n", len(handle))
	if len(handle) < (unix.SizeofInt + unix.SizeofInt) {
		return unix.FileHandle{}, []byte{}, fmt.Errorf("Invalid handle length %d", len(handle))
	}
	handle_header := (*file_handle)(unsafe.Pointer(&handle[0]))
	//fmt.Printf("Handle Header: %v\n", handle_header)

	if (unsafe.Sizeof(*handle_header) + uintptr(handle_header.handle_bytes)) > uintptr(len(handle)) {
		return unix.FileHandle{}, []byte{}, fmt.Errorf("Handle with data too short %d > %d", unsafe.Sizeof(*handle_header)+uintptr(handle_header.handle_bytes), len(handle))
	}
	start := unsafe.Sizeof(*handle_header)
	end := start + uintptr(handle_header.handle_bytes)
	//size := handle_header.handle_bytes
	//fmt.Printf("Attemptingto construct slice [%d:%d:%d] from %d bytes\n", start, end, size, len(handle))

	handle_data := handle[start:end]
	remaining = handle[unsafe.Sizeof(*handle_header)+uintptr(handle_header.handle_bytes):]
	return unix.NewFileHandle(handle_header.handle_type, handle_data), remaining, nil
}

type fanotify_event_info_fid struct {
	fanotify_event_info_header
	fid unix.Fsid
}

func asciify(b []byte) string {
	result := ""
	for _, c := range b {
		if c < 32 || c > 126 {
			result += fmt.Sprintf("\\x%02x", c)
		} else {
			result += string(c)
		}
	}
	return result
}

func parse_DFID_NAME(mountFD int, data []byte) (dirpath *string, filename string, err error) {
	//fmt.Printf("Parsing DFID_NAME: %d bytes\n", len(data))
	if uintptr(len(data)) < unsafe.Sizeof(fanotify_event_info_fid{}) {
		return nil, "", fmt.Errorf("Data too short")
	}
	data_ptr := unsafe.Pointer(&data[0])
	header := (*fanotify_event_info_header)(data_ptr)
	if header.info_type != unix.FAN_EVENT_INFO_TYPE_DFID_NAME {
		return nil, "", fmt.Errorf("Invalid info type")
	}
	fid := (*fanotify_event_info_fid)(unsafe.Pointer(&data[0]))
	//fmt.Printf("info_fid:  %v\n", fid)
	file_handle_data := data[unsafe.Sizeof(*fid):]
	//fmt.Printf("File handle data: %d bytes\n", len(file_handle_data))
	file_handle, remaining, err := NewFileHandle(file_handle_data)
	if err != nil {
		fmt.Printf("Error constructing file handle: %s\n", err)
		return nil, "", err
	}
	//fmt.Printf("Remaining bytes: %d\n", len(remaining))
	sz := len(remaining)
	for i, b := range remaining {
		if b == 0 {
			sz = i
			break
		}
	}
	filename = unsafe.String(&remaining[0], sz)
	//fmt.Printf("Filename: %s\n", filename)

	fd, err := unix.OpenByHandleAt(mountFD, file_handle, unix.O_RDONLY)
	if err != nil {
		//fmt.Printf("Error Type: %T\n", err)
		errno, ok := err.(unix.Errno)
		if ok {
			if errno == unix.ESTALE {
				return nil, filename, nil
			}

			if errno == unix.ENOMEM {
				color.Red("#### Kernel memory allocation error\n")
				return nil, filename, nil
			}
		}

		fmt.Printf("Error opening file by handle: %s\n", err)
		return nil, "", err
	}
	defer unix.Close(fd)
	linkpath := fmt.Sprintf("/proc/self/fd/%d", fd)
	_dirpath, err := os.Readlink(linkpath)
	dirpath = &_dirpath
	if err != nil {
		return nil, "", err
	}

	return dirpath, filename, nil
}

func byteSlice(ptr unsafe.Pointer, sz int) []byte {
	return unsafe.Slice((*byte)(ptr), sz)
}

func process_event_info(mountFD int, info_itor unsafe.Pointer, bufferSize int) (dirpath *string, filename string, nextInfo unsafe.Pointer, remainingBufferSize int) {
	if info_itor == nil || bufferSize == 0 {
		return nil, "", nil, 0
	}
	header := (*fanotify_event_info_header)(info_itor)

	switch header.info_type {
	case unix.FAN_EVENT_INFO_TYPE_DFID_NAME:
		//fmt.Println("FAN_EVENT_INFO_TYPE_DFID_NAME")
		_dirpath, _filename, err := parse_DFID_NAME(mountFD, byteSlice(info_itor, int(header.len)))
		if err != nil {
			fmt.Printf("Error parsing DFID_NAME: %s\n", err)
		} else {
			if _dirpath != nil {
				fmt.Printf("Filepath: %s/%s\n", *_dirpath, _filename)
			} else {
				fmt.Printf("Filepath: !!!NODIRPATH!!!/%s\n", _filename)
			}
			dirpath = _dirpath
			filename = _filename
		}
	case unix.FAN_EVENT_INFO_TYPE_FID:
		//fmt.Println("FAN_EVENT_INFO_TYPE_FID")
	default:
		fmt.Printf("Unknown info type: %d\n", header.info_type)
	}

	remainingBufferSize = bufferSize - int(header.len)
	nextInfo = nil
	if uintptr(remainingBufferSize) > unsafe.Sizeof(fanotify_event_info_header{}) {
		nextInfo = unsafe.Pointer(uintptr(info_itor) + uintptr(header.len))
	}
	return dirpath, filename, nextInfo, remainingBufferSize
}

func (w *FSWatcher) ReadEvents() ([]Event, error) {
	var buff [4096]byte
	print("\n\nReading events\n")
	n, err := w.file.Read(buff[:])
	fmt.Printf("Read %d bytes\n", n)
	if err != nil {
		return nil, err
	}
	return w.ParseEventBuffer(buff[:n])
}
func (w *FSWatcher) allowPath(path *string) bool {
	if w.filterResults {
		if path == nil {
			return false
		}
		if len(*path) >= len(w.comparePathPrefix) && w.comparePathPrefix == (*path)[:len(w.comparePathPrefix)] {
			return true
		}
		if *path == w.comparePath {
			return true
		}
		return false
	}
	return true
}

func (w *FSWatcher) ParseEventBuffer(buff []byte) ([]Event, error) {
	n := len(buff)
	var metadata_itor unsafe.Pointer = unsafe.Pointer(&buff[0])
	results := make([]Event, 0)
	for FAN_EVENT_OK(metadata_itor, n) {
		metadata := (*unix.FanotifyEventMetadata)(metadata_itor)
		if metadata.Vers != unix.FANOTIFY_METADATA_VERSION {
			offset := uintptr(unsafe.Pointer(metadata_itor)) - uintptr(unsafe.Pointer(&buff[0]))
			panic(fmt.Sprintf("Invalid metadata version: %d at offset %d in buffer of size: %d", metadata.Vers, offset, len(buff)))
		}
		if metadata.Mask&unix.FAN_Q_OVERFLOW != 0 {
			fmt.Println("!!!!!!!! Queue overflow !!!!!!!")
			continue
		}
		event, err := parseEventMask(metadata.Mask)
		if err != nil {
			panic(err)
		}
		fmt.Printf("Event: %v\n", event)
		metadata_bytes_left := int(metadata.Event_len) - int(FAN_EVENT_METADATA_LEN)
		info_itor := unsafe.Pointer(nil)
		info_count := 0
		if uintptr(metadata_bytes_left) > unsafe.Sizeof(fanotify_event_info_header{}) {
			info_itor = unsafe.Pointer(uintptr(metadata_itor) + FAN_EVENT_METADATA_LEN)
			info_count++
		}
		var dirpath *string = nil
		filename := ""
		for info_itor != nil {
			var _dirpath *string
			var _filename string
			_dirpath, _filename, info_itor, metadata_bytes_left = process_event_info(w.fs_fd, info_itor, metadata_bytes_left)
			if _filename != "" {
				dirpath = _dirpath
				filename = _filename
			}
		}
		if filename != "" {
			if w.allowPath(dirpath) {
				results = append(results, Event{EventMask: metadata.Mask, Dirpath: dirpath, Filename: filename})
			} else {
				if dirpath != nil {
					fmt.Printf("Ignoring event for %s/%s\n", *dirpath, filename)
				} else {
					fmt.Printf("Ignoring event for !!!NODIRPATH!!!/%s\n", filename)
				}
			}
		} else {
			panic("No filepath")
		}
		//fmt.Printf("Processed %d info items\n", info_count)
		n -= int(metadata.Event_len)
		if n < 0 {
			panic(fmt.Sprintf("Negative buffer size: %d", n))
		}
		if n == 0 {
			break
		}
		metadata_itor = unsafe.Pointer(uintptr(metadata_itor) + uintptr(metadata.Event_len))
	}
	return results, nil
}
