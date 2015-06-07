package media

import (
	"testing"
)

func TestExsistence(t *testing.T) {
	disp, err := NewDisplay("Camera", 640, 400)
	if err != nil {
		t.Fatal(err)
	}
	if err = disp.Open(); err != nil {
		t.Fatal(err)
	}
	disp.Close()
}
