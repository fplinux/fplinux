# FPLinux

FPLinux runs Linux on selected keypad phones. It loads over USB into RAM (temporary memory) without changing the phone's internal storage. After turning the phone off, you can start its original system normally.

Files saved in RAM are lost when the phone turns off. If you use a microSD card, follow the [shutdown instructions](https://fplinux.github.io/fplinux/use/power/) before disconnecting power.

## Documentation

[The FPLinux website](https://fplinux.github.io/fplinux/) has setup instructions, supported phone models and developer documentation.

- [Run a downloaded ZIP](https://fplinux.github.io/fplinux/guides/standalone-archive/)
- [Build FPLinux](https://fplinux.github.io/fplinux/start/build/)
- [Run from the source checkout](https://fplinux.github.io/fplinux/start/run/)
- [Phone support](https://fplinux.github.io/fplinux/phones/)
- [Contributing](https://fplinux.github.io/fplinux/develop/contributing/)
- [Bring up a new phone](https://fplinux.github.io/fplinux/porting/bring-up/)

To edit the documentation, start in `site/src/content/docs/`. See [Preview this site](https://fplinux.github.io/fplinux/develop/contributing/#preview-this-site) to check your changes locally.

## Source layout

The [porting guide](https://fplinux.github.io/fplinux/porting/) explains how the code is organized and where to add support for another phone.

## License

Original FPLinux code and documentation use [GPL-2.0-only](LICENSE) unless a file states otherwise. Third-party components retain their licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
