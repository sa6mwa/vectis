// Generate original two-pixel fixtures. This is test data preparation only.
package main

import (
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"os"
	"path/filepath"
)

func main() {
	root := filepath.Join("tests", "fixtures", "pdf")
	if err := os.MkdirAll(root, 0755); err != nil {
		panic(err)
	}
	pixels := image.NewRGBA(image.Rect(0, 0, 2, 1))
	pixels.Set(0, 0, color.RGBA{255, 0, 0, 255})
	pixels.Set(1, 0, color.RGBA{0, 0, 255, 255})
	for _, format := range []string{"png", "jpg"} {
		file, err := os.Create(filepath.Join(root, "pixels."+format))
		if err != nil {
			panic(err)
		}
		if format == "png" {
			err = png.Encode(file, pixels)
		} else {
			err = jpeg.Encode(file, pixels, &jpeg.Options{Quality: 90})
		}
		if err != nil {
			panic(err)
		}
		if err = file.Close(); err != nil {
			panic(err)
		}
	}
}
