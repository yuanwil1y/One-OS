# Host test stubs

These headers and this source file are compiled **only** by the host test groups. They
are never part of the firmware build: `tests/host/stubs` is not on the firmware include
path, so the firmware resolves every one of these names to the real component.

## `esp_err.h`

A subset of ESP-IDF's `esp_err.h` with the **same numeric values**, so a host test that
compares an error code to a named constant is testing the relation the firmware relies
on. Only the codes the host groups use are declared; adding one is a one-line change
with no behaviour attached.

## `theengs_l2.h` and `zha_l2.h`

`firmware/main/app_recognition.c` answers two product questions by **asking the family
that owns the id space** rather than keeping a duplicated list in the application:

- `app_decoder_is_available()` → `theengs_model_info()`
- `app_quirk_is_available()` → `zha_quirk_get_info()`

That is the right dependency in the firmware build, where both components are linked.
The host groups `app_device` and `app_device_db` do not otherwise build those components,
and pulling in the whole families for one lookup would make those groups depend on code
they are not testing. These two headers therefore reproduce only the declarations needed,
with the same values as the real component headers.

`app_l2_lookup_stub.c` supplies the matching definitions for the ids the fixture corpus
names. It is a test double: any test that depends on its answer states the expectation
explicitly (`test_decoder_availability_is_real` in `test_app_device_db.c`).

**These stubs can drift from the components they mirror.** They are short, they are
reviewed whenever a component header changes, and the firmware build fails immediately if
a signature diverges — which is the point: a wrong stub breaks the host test, a wrong
firmware call breaks the build.
