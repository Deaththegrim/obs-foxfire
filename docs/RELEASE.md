# Release checklist

A release is a git tag. Nothing below is optional, and the order matters: each step exists because
skipping it has a specific, known failure mode.

1. **`ctest` green.**
   ```
   cmake --preset ubuntu-x86_64 -DFOXFIRE_BUILD_TESTS=ON
   cmake --build --preset ubuntu-x86_64
   ctest --test-dir build_x86_64 --output-on-failure
   ```
   Also run the licence interop gate directly, not just via ctest — `ctest` counts a skipped test
   as a pass, and this is the only check that Python-signed licences actually verify in C:
   ```
   python3 tools/licence-interop.py build_x86_64/tests/verify_cli
   ```
   A missing `cryptography` module must exit non-zero here, not silently report green.

2. **Local proof green.**
   ```
   LIBGL_ALWAYS_SOFTWARE=1 python3 tools/proof.py --plugin-build . --pack data/packs/demo --out proof-out
   ```
   Exit 0. Check `proof-out/report.json` and `proof-out/obs.log` by hand, don't just trust the
   return code — the log line count matters as much as pass/fail (see `tools/proof.py`'s own
   warning scanner: any unexpected `[obs-foxfire] warn:`/`error:` line fails the run).

3. **CI green on Linux and Windows.** Both `proof.yaml` (Linux, runs the same proof as step 2 under
   software GL) and the template's `build-project` workflow (which builds Windows, macOS, and Ubuntu
   artefacts) must be green on the commit being tagged. Check with:
   ```
   gh run list --workflow proof.yaml
   gh run list --workflow build-project.yaml
   ```
   `test_handoff.c`'s `reads > 1000` check is a known timing-sensitive flake on CI runners (not on
   this dev box). If only that assertion fails, rerun the job before treating it as a real
   regression — do not weaken or delete the check to make a run go green.

4. **Windows artefact smoke-tested by a Windows user.** Download the Windows zip CI produced,
   install it (see README — installs to `C:\ProgramData\obs-studio\plugins\obs-foxfire\`), open OBS,
   add a "Foxfire Visualizer" source, pick the Demo pack, pick the Bars preset, and confirm it
   renders. **The draft release is not published until this step has actually been done by someone
   on real Windows hardware** — nothing about a green Windows CI job proves the plugin loads or
   renders correctly there; CI only proves it built.

5. **The public key must be baked in before any release that ships a paid pack.**
   `src/ff-pack.c`'s `FF_PUBLIC_KEY` is still 32 zero bytes as of this writing — packforge (a
   separate, private tool) generates the real key and it gets baked in as `src/ff-public-key.h`
   in a later task. Until that lands, every release the engine produces will log
   `licence: public key not set; paid packs will not verify` on every startup, and any paid pack's
   licence will fail to verify — full stop, no partial verification, no fallback. A release of the
   free demo pack alone doesn't need the key. A release meant to carry a paid pack needs the key
   baked in first, and this checklist re-run after that lands, before that release ships.

6. **Tag `x.y.z`.**
   ```
   git tag 0.1.0
   git push --tags
   ```
   Semantic versioning (`git tag 12.3.4`, or `23.4.5-beta2` for a pre-release). The template's
   release workflow builds and attaches the Windows zip and the Ubuntu package to a **draft**
   release automatically — it does not publish it.

7. **Draft release published with both artefacts.** Confirm both the
   `obs-foxfire-<version>-windows-x64.zip` and the Ubuntu package are attached
   (`gh release view <version>`), then publish the draft — only after step 4 has actually happened,
   not before.

8. **Gear page download link updated.** `kitsunestudio`'s Gear page points at the published release;
   update it to the new tag once the release above is live.
