package main

import (
	"flag"
	"fmt"
	"net"
	os "os"
	"os/signal"

	"github.com/fatih/color"

	"github.com/walterschell/clamav-dbscan/watcher/fanotify"
)

const DEFAULT_UNIX_SOCKET_PATH = "/tmp/fswatcher.sock"

type UnixDomainServer struct {
	socket net.Listener
}

func (s *UnixDomainServer) Close() {
	s.socket.Close()
}

func NewUnixDomainServer(path string, watcher *fanotify.FSWatcher) (*UnixDomainServer, error) {

	socket, err := net.Listen("unixpacket", path)
	if err != nil {
		if err.(*net.OpError).Err.Error() == "bind: address already in use" {
			//TODO: check if the socket is still alive
			client, err := net.Dial("unixpacket", path)
			if err == nil {
				client.Close()
				return nil, fmt.Errorf("socket %s is already actively in use", path)
			}
			fmt.Printf("Removing stale socket %s\n", path)
			err = os.Remove(path)
			if err != nil {
				return nil, fmt.Errorf("failed to remove stale socket %s: %v", path, err)
			}
			fmt.Printf("Removed stale socket %s\n", path)
			_, err = os.Stat(path)
			if err == nil {
				return nil, fmt.Errorf("failed to remove stale socket %s: %v", path, err)
			}
			socket, err = net.Listen("unixpacket", path)
			if err != nil {
				return nil, fmt.Errorf("failed to listen (again) on %s: %v", path, err)
			}
		} else {
			return nil, fmt.Errorf("failed to listen on %s: %v", path, err)
		}
	}
	err = os.Chmod(path, 0666)
	if err != nil {
		socket.Close()
		return nil, err
	}

	go func() {
		for {
			conn, err := socket.Accept()
			if err != nil {
				if err.(*net.OpError).Err.Error() == "use of closed network connection" {
					return
				}
				fmt.Println(err)
				continue
			}
			color.Green("Accepted connection %v\n", conn)

			go func(conn net.Conn) {
				defer conn.Close()
				subscription := watcher.AddJsonSubscription()
				defer subscription.Close()
				for {
					select {
					case events := <-subscription.Channel():
						sz, err := conn.Write(events)
						if err != nil {
							fmt.Println(err)
							color.Yellow("Closing connection %v\n", conn)
							return
						}
						if sz != len(events) {
							color.Red("Failed to write all events: %d != %d\n", sz, len(events))
						}

					}
				}
			}(conn)
		}
	}()
	return &UnixDomainServer{socket}, nil
}

func main() {
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Printf("Invalid number of arguments: %d\n", flag.NArg())
		fmt.Println("Usage: watcher [flags] <path>")
		flag.PrintDefaults()
		os.Exit(1)
	}

	path := flag.Arg(0)
	watcher, err := fanotify.NewFSWatcher(path, true)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	server, err := NewUnixDomainServer(DEFAULT_UNIX_SOCKET_PATH, watcher)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	defer server.Close()
	ch := make(chan os.Signal, 1)
	defer close(ch)
	signal.Notify(ch, os.Interrupt)
	<-ch
}
