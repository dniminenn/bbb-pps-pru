# Diagrams

Block diagrams are authored in [D2](https://d2lang.com) (the `.d2` sources) and rendered
to SVG. The SVGs carry both a light and a dark theme and switch with the viewer's color
scheme. Regenerate after editing a source:

```sh
for f in *.d2; do d2 --theme 0 --dark-theme 200 --pad 24 "$f" "${f%.d2}.svg"; done
```
