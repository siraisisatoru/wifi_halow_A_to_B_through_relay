<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# mm-iot-sdk patches

Fixes carried in this repository for bugs that are still present in mm-iot-sdk **2.14.3**. Issue
numbers refer to the reports on [MorseMicro/esp-halow](https://github.com/MorseMicro/esp-halow/issues).
All three bugs are in `framework/morselib`, which both builds compile from source: the EKH05 relay
uses the SDK make flow (`BUILD_MORSELIB_FROM_SOURCE=y` by default), and esp-halow compiles
`components/mm-iot-sdk/framework/morselib/src/...` in `halow/components/morselib/CMakeLists.txt`.
The patches therefore have to be applied to **both** SDK checkouts.

| Patch | Issue | Affects | Symptom |
|---|---|---|---|
| `mm-iot-sdk/0001-umac_ap-fix-STA-record-use-after-free-on-removal.patch` | #9 | any AP, including relay root and repeater | heap use after free on STA removal. With relay enabled it happens on every removal and crashes the AP on the first disassociation. In any AP mode it also happens when an ADDBA, RX reorder or defrag timeout is still pending at removal |
| `mm-iot-sdk/0002-umac_relay-send-root-group-addressed-frames-on-AP-VIF.patch` | #10 | relay root | the root cannot transmit any broadcast or multicast frame, so it cannot ARP for anyone |
| `mm-iot-sdk/0003-umac_connection-fix-zero-passphrase_len-handling.patch` | #11 | STA and relay repeater | with `passphrase_len = 0` the SAE password is cut to `strlen(ssid)` |

The patches are `git format-patch` output against the `2.14.3` tag. Each one stands alone, and
they apply in any order. 0001 touches `umac_ap.c`, `umac_ba.c` and `umac_datapath.c`; the
others touch one file each.

## Applying

```sh
tools/apply_sdk_patches.sh            # apply to the default SDK trees
tools/apply_sdk_patches.sh --check    # report state only, change nothing
tools/apply_sdk_patches.sh --revert   # undo
tools/apply_sdk_patches.sh --unhide   # show the patched files as modified again (see below)
tools/apply_sdk_patches.sh path/to/mm-iot-sdk [...]   # explicit SDK roots
```

The default trees are `third_party/mm-iot-sdk` and, if it is checked out,
`third_party/esp-halow/halow/components/mm-iot-sdk`. Run the script after every
`git submodule update`, because updating a submodule throws away the patched working tree.

Each patch is handled separately:

- If `git apply -R --check` succeeds, the patch is already applied and is skipped.
- Otherwise, if `git apply --check` succeeds, the patch is applied.
- Otherwise the script stops with an error and prints the `git apply` output.

Patches applied before a failure stay applied, and running the script again is safe. Only the
working tree is changed, never a commit: the submodules still point at the pristine upstream
commit. `--check` exits non-zero if any patch would conflict, so it works as a CI gate.

**Patched files are hidden from git.** After a successful apply, the script marks the patched
files `skip-worktree` in the SDK's git index (a local flag, never committed). `git status` in the
SDK, in esp-halow and in this repository, and editors such as VS Code, then show the submodules as
clean, although the files on disk are patched. Consequences:

- `git diff` in the SDK does not show the patches. Use `--check` to see the state, or `--unhide`
  to make the files show as modified again (apply hides them again).
- A real local edit to one of those five files is hidden too. `--check` still catches it,
  because it compares file contents, and so does the relay's link-time check.
- Run `--revert` (which clears the flags) before checking out another SDK version, so that
  `git checkout` / `git submodule update` can update those files.

The script also works on an SDK copy that is not its own git repository: a plain directory, or a
sub-directory of another repository. For the second case it passes `--directory` to
`git apply`.

### Regenerating a patch

```sh
git -C third_party/mm-iot-sdk worktree add --detach /path/to/wt 2.14.3
cd /path/to/wt && git am /abs/path/patches/mm-iot-sdk/*.patch
# edit, then: git commit --amend   (or git rebase -i 2.14.3)
git format-patch --no-signature --zero-commit -o /abs/path/patches/mm-iot-sdk 2.14.3..HEAD
git -C /abs/path/third_party/mm-iot-sdk worktree remove --force /path/to/wt
```

`format-patch` names files after the subject line. Rename them back to the names above so the
references in this file stay valid.

---

## 0001: #9, use after free in `umac_ap_remove_sta_record()`

The function frees the STA record while two kinds of reference to it are still live: the removal
notification (part 1, the reported #9 crash) and armed core timeouts (part 2, found while
reviewing the first fix).

**Root cause, part 1: the notification.** In `framework/morselib/src/umac/ap/umac_ap.c`, `umac_ap_remove_sta_record()`
(lines 917-965) frees the record at **line 953** (`mmosal_free(stad)`) and then calls
`umac_ap_notify_sta_status_change(stad, ...)` at **line 958**. That function passes `stad` to the
relay hook at line 872, and `umac_relay_handle_ap_sta_state()`
(`src/umac/relay/umac_relay.c` lines 829-861) dereferences it:

- line 832: `umac_sta_data_get_umacd(stad)`
- line 859: `umac_interface_peek_mac_addr(stad)` (`interface/umac_interface.c:514`), which reads
  the umacd and the VIF ID from the record
- line 708, in `umac_relay_process_reachable_address_update()`:
  `umac_sta_data_peek_peer_addr(stad)`, which supplies the next-hop address

Without relay mode the hook returns at line 834 before touching anything else, which is why
plain AP mode rarely shows the bug. With relay enabled, every removal reads freed memory and
then inserts into, or removes from, the relay table based on that memory.

**Callers checked.** All callers reach the bug:

- `umac_ap_remove_sta()` (line 967), via `mmwpas_sta_remove()` in
  `supplicant_shim/driver_ap.c:532`. hostapd calls this for deauth, disassociation, inactivity
  and failed authentication.
- `umac_ap_stop()` (line 1303). This path has a second use after free: `data->stas[ii]` still
  points at the record until line 1304, so `dump_sta_list()` at line 962 (built only at
  `MMLOG_LEVEL_VRB`) walks the freed record.

The other free of a record, `umac_ap_dealloc_sta()` at line 801 (when `umac_ap_add_sta()`
fails), sends no notification. No caller uses `stad` after `umac_ap_remove_sta_record()`
returns, and the relay table stores MAC addresses, not `stad` pointers.

**Root cause, part 2: pending core timeouts.** The record is a single allocation
(`umac_sta_data_alloc()`, `data/umac_data.c`) that embeds the BA state and the per-STA datapath
state. Before the free, the function tears down only rate control and the TX queue
(`umac_datapath_stad_flush_txq()`, line 950). Nothing on the AP path cancels the three core
timeouts whose arguments are the record or point into it:

- `umac_ba_addba_req_timeout_handler(stad, &session)` (`ba/umac_ba.c:393`). It is armed when
  the AP sends unicast data to an authorized station with AMPDU enabled (the default,
  `config/umac_config.c:61`), with a backoff of 100 ms doubling to 60 s. It is cancelled only when
  an ADDBA response or DELBA arrives (`umac_ba.c:131/205`). When it fires it reads
  `session->status` and transmits a DELBA through `stad`.
- `umac_datapath_rx_reorder_timeout_handler(umacd, stad)` (`datapath/umac_datapath.c:662/691`),
  armed whenever an out-of-order MPDU is buffered. Nothing ever cancels it.
- `datapath_defrag_timeout(frag_chain)` (`datapath/datapath_defrag.c:70`), with `frag_chain`
  embedded in the record. It is cancelled only by `datapath_defrag_deinit()`, which the AP path
  never calls.

This part does not need relay mode. A typical trigger: node A goes silent while B keeps
retransmitting TCP to it, so every AP transmission to A retries ADDBA with a growing backoff,
and hostapd's inactivity or deauth path then removes A with the timeout still armed. An
out-of-order MPDU from A buffered less than `RX_REORDER_TIMER_PERIOD_MS` (25 ms) before removal
does the same, which is common on a lossy hop. The mmpkts in the RX reorder list and in the
fragment chains were also leaked on every removal, which slowly drains the RX pool.

**Fix.**

- Move `mmosal_free(stad)` to the very end of `umac_ap_remove_sta_record()`, after the
  notification and the debug dump.
- Replace `umac_datapath_stad_flush_txq()` with the existing `umac_datapath_stad_flush()`, the
  call the STA disconnect path already makes (`connection/umac_connection.c:1967`). It drains the
  RX reorder list, runs `datapath_defrag_deinit()` (which releases the chains and cancels their
  timeouts) and flushes the TX queue. The patch makes it also cancel the RX reorder timeout for
  the record.
- Call `umac_ba_deinit()`, which the patch makes cancel the ADDBA request timeouts of every TID
  for the record (`UMAC_CORE_TIMEOUT_ANY_CTX` on the session argument) before clearing the BA
  state.

After this, none of the timeouts registered anywhere in morselib (checked by listing every
`umac_core_register_timeout()` call) takes the record, or a pointer into it, as an argument
once the function returns.

**Risk.** Low. The driver update, rate control teardown, TIM bit clear and callback order are
unchanged. The one behaviour change on the AP path is that frames still waiting in the RX reorder
list are delivered (in order) instead of leaked, exactly as on the STA disconnect path. They are
delivered before the relay hook runs, while the record is still valid. The application's
`sta_status_cb` still runs before the relay hook and never receives `stad`. The STA disconnect
path calls the same two functions, so it also stops leaving stale reorder and ADDBA timeouts
behind; its record is never freed, so for it this is only tidier.

**App-level workaround.** None is practical. The crash happens inside morselib on any
disassociation, so the patch is required on the relay (EKH05) and on node B (ESP root). It is
also worth applying on any plain AP build because of part 2.

**Dropping it.** Upstream is fixed when the free comes after `umac_ap_notify_sta_status_change()`
in `umac_ap_remove_sta_record()` and the function tears down the reorder, defrag and BA state
(and their timeouts) before freeing. After an SDK bump, if `--check` reports CONFLICT for 0001,
inspect that function, `umac_datapath_stad_flush()` and `umac_ba_deinit()`. Delete the patch if
both parts are fixed, or rebase the remaining part if not.

## 0002: #10, a relay root cannot send group-addressed frames

**Root cause.** Every frame from the host stack passes through
`umac_relay_update_tx_metadata()` (`src/umac/relay/umac_relay.c` lines 894-952, called from
`mmwlan_tx_pkt()` at `src/umac/umac.c:1508`). A frame goes to the AP VIF only if its DA is an
associated STA (line 919) or is in the relay table (line 931). Everything else, including every
multicast and broadcast DA, falls through to `metadata->vif = MMWLAN_VIF_STA` at
**lines 945-949**. A root (`depth == 0`, set at line 334 when `root_ap_args` is given; `is_root()`
is at line 35) has no STA VIF. `mmwlan_tx_pkt()` therefore drops these frames with
`MMWLAN_VIF_ERROR` ("No matching VIF", `umac.c:1546`). The root cannot send ARP requests, DHCP,
mDNS or IPv6 ND. It is reachable only for as long as each peer's own ARP traffic keeps the root's
ARP cache populated.

**Fix.** On the root, send all remaining frames on the AP VIF, which is the upstream-suggested
fix. Group-addressed frames then use the BSS common STA record (`umac_ap_lookup_sta_by_addr()`
returns `sta_common` for multicast, `ap/umac_ap.c:735-738`), exactly as in plain AP mode. A
unicast DA that is neither associated nor in the relay table is now dropped by the AP datapath
("No STA record", `MMWLAN_NOT_FOUND`, `datapath/umac_datapath.c:1924-1928`) instead of failing
VIF lookup. That frame was undeliverable either way.

**Repeaters: considered but not changed.** A repeater's own group-addressed frames, such as its
ARP requests, still go only upstream. In morselib's RX path they still reach the repeater's own
BSS:

1. The repeater sends the broadcast to the root on its STA VIF.
2. The root's `umac_relay_process_data_frame()` (multicast received on an AP VIF at a root, the
   `else` branch at lines 579-589) forwards it on the root's AP VIF and also passes it up.
3. The repeater receives that copy on its STA VIF. The same branch forwards it down its AP VIF
   to its stations, such as node A, and marks it consumed because the SA is its own (lines
   585-588). It is never delivered back to its own stack (see also
   `datapath/umac_datapath.c:522-526`).

Duplicating the frame onto the repeater's AP VIF would therefore give its stations every
broadcast twice. The copies are separate transmissions with their own sequence numbers and
PNs, so stations would not discard the second one. That change is not clearly correct, so it is not implemented. The only cost is a dependency: **while the repeater's
upstream link is down, its own broadcasts reach nobody.** Node A then cannot learn the relay's
MAC by ARP, and the relay cannot learn A's. Pinging the relay from A is a diagnostic that needs
the root to be up. Our plan is single-subnet with static IPs, so the app-level mitigation is
static ARP entries between A and the relay if that matters. The relay's IP stack uses the
**STA VIF MAC** (the device MAC), not the fixed AP BSSID `02:48:4C:00:00:02` (see #7 below).

This firmware behaviour was not verified on hardware: whether the MM6108 firmware itself drops
received frames whose addr3/SA equals its own MAC. If it does, the repeater's own broadcasts
never reach its BSS, and static ARP between A and the relay becomes required rather than
optional.

**Risk.** Low. The change only affects `is_root()` nodes and only frames that previously failed
with `MMWLAN_VIF_ERROR`. Root forwarding of relayed traffic does not use this function (it calls
`umac_datapath_tx_frame()` directly) and is unchanged.

**App-level workaround without the patch.** On the root, add static ARP entries for every peer.
With this patch none are needed, and the network plan uses plain dynamic ARP.

**On the ESP root (node B) this is already worked around in the glue.** esp-halow
`sdk-2.14.3-idf6` at `3c46c92` sets `metadata.ra` to the group DA for every group-addressed frame
sent while relay mode is active on the AP VIF (`halow_transmit()`, `halow/mmhalow.c:97-109`; on a
root `mmhalow_relay_enable()` sets both `halow_relay_active` and `halow_ap_active`,
`mmhalow.c:519-523`). `umac_relay_update_tx_metadata()` returns early when an RA is supplied
(`umac_relay.c:904-908`), so the branch this patch changes never runs for node B's own frames.
For node B, 0002 is defence in depth against that glue changing; do not treat it as what makes
node B's ARP work, and test node B's broadcasts with the glue as it is. 0002 is required for
mmipal-based roots (mmnetif never supplies an RA, see #7 below), such as an EKH05 configured as
the root.

**Dropping it.** Upstream is fixed when `umac_relay_update_tx_metadata()` no longer tags frames
for `MMWLAN_VIF_STA` on a root. Check this after each SDK bump.

## 0003: #11, `passphrase_len == 0` is replaced by `strlen(ssid)`

**Root cause.** `struct mmwlan_sta_args.passphrase_len` is documented as "may be zero if
passphrase is null-terminated" (`include/mmwlan.h:1148-1149`), and
`umac_connection_validate_sta_args()` (`src/umac/connection/umac_connection.c:79-108`) validates
the passphrase on that basis. However, `umac_connection_start()` stores
`strlen((const char *)data->sta_args.ssid)` at **line 365**. `config_populate_network_security()`
(`supplicant_shim/config.c:62-73`) then copies exactly that many bytes into `sae_password`:

- If the SSID is shorter than the passphrase, the password is truncated. With our defaults, the
  15-character `halow-relay-lab` truncates `change-me-halow-au` to `change-me-halow`, so SAE fails
  against a correctly configured AP.
- If the SSID is longer, the copy includes the passphrase's NUL and the bytes after it. It
  only works because the embedded NUL ends the string early.
- A 32-byte SSID has no NUL in `ssid[]`, so the `strlen()` itself over-reads.

**Relay-specific effect.** `umac_relay_start_relay_ap()` copies `sta_args.passphrase_len`
(including the already-corrupted value) into the repeater AP's `mmwlan_ap_args`
(`relay/umac_relay.c:236`). The repeater's downstream BSS then also runs with the wrong SAE
password, so node A fails to authenticate to the relay even if node A itself sets the length
correctly.

**Fix.** Use `strnlen(passphrase, MMWLAN_PASSPHRASE_MAXLEN)`. The bound matches the validator,
which already guarantees a terminator within `MMWLAN_PASSPHRASE_MAXLEN + 1` bytes.

**Risk.** Negligible. Callers that set `passphrase_len` explicitly see no change.

**App-level workaround (keep it even with the patch).** Always set `passphrase_len` explicitly in
`mmwlan_sta_args` (relay STA side, node A) and `mmwlan_ap_args` (node B root), for example
`args.passphrase_len = strlen(args.passphrase);`. This also protects builds made from an
unpatched SDK.

**Dropping it.** Upstream is fixed when line 365 uses the passphrase. The app workaround is
harmless either way.

---

## Investigated, not patched

### #9 part 2: static ARP entry removed on an ESP root when a second STA associates

No call path in mm-iot-sdk 2.14.3 or in esp-halow removes a static ARP entry when a STA is
added, so there is nothing to patch with certainty. Findings:

- **mm-iot-sdk.** The only `etharp_remove_static_entry()` call outside lwIP itself is
  `mmipal_remove_static_arp_entry()` (`framework/src/mmipal/lwip/mmipal_lwip.c:244`). Its only
  in-tree caller is the EMMET remote-control command handler
  (`framework/src/emmet/emmet_lwip.c:931`). Nothing on the AP STA-add or STA-remove path
  (`umac_ap_add_sta()`, `umac_ap_update_sta()`, `mmwpas_sta_add()` and `mmwpas_sta_remove()`)
  touches lwIP. esp-halow does not use mmipal anyway.
- **esp-halow.** Commit `3c46c92` on `sdk-2.14.3-idf6` has no ARP calls in `halow/*.c` or the
  examples. It creates its netif with `ESP_NETIF_DEFAULT_WIFI_STA()` (DHCP client flag), and maps
  `mmwlan` link state to `esp_netif_action_connected()` and `esp_netif_action_disconnected()`
  (`mmhalow_link_state()`, `halow/mmhalow.c:22-36`). In relay mode, `mmhalow_relay_enable()`
  replaces the link-state callback with a VIF-state callback, `mmhalow_relay_vif_state()`
  (`mmhalow.c:447-459`), that drives the netif from one VIF only: the AP VIF on the root, the
  relay STA VIF elsewhere (`relay_link_vif`, `mmhalow.c:511`).
- **ESP-IDF v6.0.2, plausible cause A: DHCP server.**
  `components/lwip/apps/dhcpserver/dhcpserver.c` adds a temporary static ARP entry for the
  offered or acknowledged IP (lines 645, 654 and 661, in `dhcps_response_ip_set()`). After
  sending OFFER, NAK or ACK it calls `etharp_remove_static_entry()` for that IP (lines 760, 861
  and 941). Suppose a root runs the ESP DHCP server and an already-associated STA has a static
  ARP entry for an IP the server then offers to the second STA (a static IP inside the pool, or
  a re-offered lease). The second STA's DHCP exchange then overwrites the first STA's entry and
  deletes it. This matches "removed when a 2nd STA associated".
- **ESP-IDF v6.0.2, plausible cause B: link-down flush.** `esp_netif_action_disconnected()` calls
  `esp_netif_down()` → `netif_set_down()` (`components/esp_netif/lwip/esp_netif_lwip.c:1936`).
  lwIP's `netif_set_down()` calls `etharp_cleanup_netif()`
  (`components/lwip/lwip/src/core/netif.c:977`), which frees every ARP entry on the netif,
  **including static ones**, and also clears the IPv4 address (`esp_netif_lwip.c:1934`). In
  morselib the link-state callback fires only for the STA connection
  (`umac_connection.c:2036/2081`), not for AP STA events. But if any glue, such as the new esp-halow
  relay plumbing, maps AP, relay or VIF events onto `esp_netif_action_disconnected()` or
  `esp_netif_action_connected()`, every such event wipes all ARP entries on the root.

**Guidance for this project:**

1. Do not run a DHCP server on node B. The plan is static IPs only.
2. On an AP or root, nothing may map per-STA events onto `esp_netif_action_disconnected()`.
   Checked against esp-halow `3c46c92`: on the root, `mmhalow_relay_vif_state()` reacts only to
   AP VIF link-state changes, and morselib reports those only when the AP starts
   (`ap/umac_ap.c:458-462`) and stops (`ap/umac_ap.c:1364-1368`), never when a station
   associates or leaves. So cause B can only flush node B's ARP table when its AP stops, not
   when a second STA associates. Re-check this if the glue changes.
3. If an app installs static ARP entries, re-install them (idempotently) from `sta_status_cb`
   on `MMWLAN_AP_STA_AUTHORIZED`, rather than once at boot. The root can ARP normally (through
   the esp-halow RA workaround on node B, or patch 0002 on an mmipal root), so static entries
   are not needed in our design.

### #7: "Unable to infer VIF ID" and mmipal on the EKH05

The SDK's mmipal/lwIP glue never triggers the inference failure, so no patch is needed.

- The inference that fails is in `mmwlan_tx_pkt()` (`src/umac/umac.c:1520-1530`). It runs only
  when `metadata.vif == MMWLAN_VIF_UNSPECIFIED` and both VIFs exist.
- In relay mode, `umac_relay_update_tx_metadata()` runs first (`umac.c:1508`) and always sets
  `vif` to AP or STA unless an explicit RA is supplied (`umac_relay.c:904-908`). mmnetif never
  supplies an RA.
- mmnetif also sets the VIF itself. It binds the netif to the first VIF that reports link-up
  (`framework/src/mmipal/lwip/mmnetif.c:134`), and every TX carries `.vif = state->vif`
  (`mmnetif.c:173`). The value is `MMWLAN_VIF_UNSPECIFIED` only before the first link-up.

Behaviour worth knowing for the relay app:

- On a repeater, the AP VIF does not report link-up (`ap/umac_ap.c:455-463` only for
  relay-disabled or root). The netif therefore binds to the **STA VIF**, with the STA (device)
  MAC address.
- Unicast addressed to that MAC that arrives on the repeater's AP VIF, for example from node A,
  is re-tagged as STA (`datapath/umac_datapath.c:528-535`). mmnetif's VIF filter
  (`mmnetif.c:62-67`) therefore accepts it.
- Group-addressed frames from node A are consumed by the relay forwarding at
  `umac_relay.c:562-576`. They reach the relay's own stack only through the root's
  retransmission. This is the same upstream dependency described under 0002.
- On the root, the netif binds to the AP VIF.

## Other notes for app authors (relay, from reading 2.14.3)

- The repeater's AP copies its SSID, security type, passphrase, PMF mode and SAE groups from the
  relay STA's `mmwlan_sta_args` (`umac_relay.c:230-238`). It uses `op_class = 0` and
  `s1g_chan_num = 0`, so it follows the upstream channel (lines 239-240). Only `bssid`, beacon
  interval, DTIM period and the STA status callback come from `mmwlan_relay_ap_args`.
- esp-halow `sdk-2.14.3-idf6` at `3c46c92` compiles the relay sources (`s1g_action.c`,
  `reachable_address.c`, `address_updates.c`, `umac_relay.c`, `umac_relay_table.c`) and defines
  `ENABLE_MMWLAN_RELAY` when `CONFIG_HALOW_RELAY_MODE` is set, and compiles `umac_ap.c` when
  `CONFIG_HALOW_AP_MODE` is set (`halow/components/morselib/CMakeLists.txt`). Patch 0001 (and
  0003) must therefore be applied to `third_party/esp-halow/halow/components/mm-iot-sdk` for
  node B, and for node A's relay-STA variant. 0002 is harmless there (see the 0002 section).

## Verification done

- All three patches applied with `git am` on a clean `2.14.3` worktree.
- The extended 0001 (part 2) was checked with `git apply --check` against a pristine `2.14.3`
  tree and built into the EKH05 relay firmware with `relay_ekh05/build.sh` (morselib from source,
  rc=0, no warnings).
- All three patches applied to `third_party/esp-halow/halow/components/mm-iot-sdk` and built into
  node_a and node_b with ESP-IDF 6.0.2 (node_b compiles the patched `umac_ap.c`, `umac_ba.c`,
  `umac_relay.c` and `umac_connection.c`; 0 warnings).
- `examples/relay_mode` and `examples/ap_mode` built for `mm-mm6108-ekh05` in
  `ekh05-builder:2.14.3` (`-Werror -Wall -Wextra`, morselib built from source). Both builds
  exited with rc=0 and produced no warnings.
- `tools/apply_sdk_patches.sh` was tested on a second fresh worktree:
  - `--check` reported all patches as not applied.
  - Apply applied all three.
  - Re-applying was a no-op.
  - The result was byte-identical to the `git am` tree.
  - `--revert` reverted all three; a second `--revert` was a no-op and left a clean tree.
  - A locally edited file was reported as CONFLICT with a non-zero exit.
  - Vendored copies inside an unrelated git repository and in a plain directory were handled.
  - shellcheck 0.10.0 reported no issues.
- No runtime testing on hardware has been done yet.
