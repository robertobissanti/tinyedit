# Test fixtures

Run all automated checks from the repository root with:

```sh
make test
```

The files in `fixtures/` are also intended for manual inspection in
tinyedit. For example:

```sh
./tinyedit tests/fixtures/demo.css
```

Check comments, strings, keywords, Unicode identifiers and multi-line
constructs. `F1`, `F2`, and `F3` should open Help, Settings, and Info.
F3 accepts both the SS3 sequence commonly emitted by terminal emulators
and the CSI `ESC[13~` form.
