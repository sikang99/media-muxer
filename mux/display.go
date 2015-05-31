package mux

/*
#cgo LDFLAGS: -lSDL
#include <libavcodec/avcodec.h>
#include <SDL/SDL.h>
*/
import "C"
import (
	"fmt"
	"unsafe"
)

type Display struct {
	screen        *C.SDL_Surface
	overlay       *C.SDL_Overlay
	frame         chan *C.AVFrame
	width, height C.int
	rendering     bool
}

func NewDisplay(title string, width, height int) (*Display, error) {
	d := Display{frame: make(chan *C.AVFrame)}
	d.width = C.int(width)
	d.height = C.int(height)
	d.screen = C.SDL_SetVideoMode(d.width, d.height, 0, 0)
	d.overlay = C.SDL_CreateYUVOverlay(d.width, d.height, C.SDL_IYUV_OVERLAY, d.screen)
	t := C.CString(title)
	C.SDL_WM_SetCaption(t, (*C.char)(null))
	C.free(unsafe.Pointer(t))
	return &d, nil
}

func (d *Display) Render(frame *C.AVFrame) {
	d.frame <- frame
}

func (d *Display) Open() error {
	if !d.rendering {
		d.rendering = true
		go d.routine()
		return nil
	}
	return fmt.Errorf("display already on")
}

func (d *Display) Close() {
	d.rendering = false
}

func (d *Display) routine() {
	rect := C.SDL_Rect{x: 0, y: 0, w: C.Uint16(d.width), h: C.Uint16(d.height)}
	for d.rendering {
		frame := <-d.frame
		C.SDL_LockYUVOverlay(d.overlay)
		pixels := (*[1 << 30]*C.Uint8)(unsafe.Pointer(d.overlay.pixels))
		pitches := (*[1 << 30]C.Uint8)(unsafe.Pointer(d.overlay.pitches))
		pixels[0] = (*C.Uint8)(frame.data[0])
		pixels[1] = (*C.Uint8)(frame.data[1])
		pixels[2] = (*C.Uint8)(frame.data[2])
		pitches[0] = (C.Uint8)(frame.linesize[0])
		pitches[1] = (C.Uint8)(frame.linesize[1])
		pitches[2] = (C.Uint8)(frame.linesize[2])
		C.SDL_UnlockYUVOverlay(d.overlay)
		C.SDL_DisplayYUVOverlay(d.overlay, &rect)
	}
}

func init() {
	C.SDL_Init(C.SDL_INIT_VIDEO | C.SDL_INIT_AUDIO | C.SDL_INIT_TIMER)
}
