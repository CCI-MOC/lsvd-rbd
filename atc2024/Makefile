.DEFAULT_GOAL := main.pdf

main.pdf: build/main.pdf
	cp $< $@

TEXFILES = $(wildcard *.tex)

build/main.pdf: $(TEXFILES)
	mkdir -p build
	latexmk -pdf -pdflatex="pdflatex -interaction=nonstopmode" -use-make main.tex -output-directory=build

clean:
	rm -rf build/*

