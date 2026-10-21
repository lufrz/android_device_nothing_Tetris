#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep host broadcast-delivery assumptions anchored to the platform being built."""

from pathlib import Path
import sys
import xml.etree.ElementTree as ET


tree = Path(sys.argv[1])
android_name = "{http://schemas.android.com/apk/res/android}name"
android_uid = "{http://schemas.android.com/apk/res/android}sharedUserId"
systemui = ET.parse(tree / "frameworks/base/packages/SystemUI/AndroidManifest.xml").getroot()
parts = ET.parse(tree / "device/nothing/Tetris/app/parts/AndroidManifest.xml").getroot()
assert systemui.get(android_uid) == "android.uid.systemui"
assert parts.get(android_uid) == "android.uid.system"
assert systemui.get(android_uid) != parts.get(android_uid)
print("PASS platform SystemUI and Parts use different shared UIDs")

platform = ET.parse(tree / "frameworks/base/core/res/AndroidManifest.xml").getroot()
protected = {element.get(android_name) for element in platform.findall("protected-broadcast")}
assert {"android.intent.action.USER_PRESENT", "android.intent.action.USER_UNLOCKED"} <= protected
print("PASS platform protects both exported unlock actions against third-party senders")

mediator = (tree / "frameworks/base/packages/SystemUI/src/com/android/systemui/keyguard/KeyguardViewMediator.java").read_text()
assert "new Intent(Intent.ACTION_USER_PRESENT)" in mediator
assert "mContext.sendBroadcastAsUser(USER_PRESENT_INTENT," in mediator
print("PASS SystemUI sends USER_PRESENT from its own context")

policy = " ".join((tree / "frameworks/base/services/core/java/com/android/server/am/BroadcastSkipPolicy.java").read_text().split())
assert "if (!filter.exported && checkComponentPermission(null, r.callingPid, originalCallingUid, filter.receiverList.uid, filter.exported) != PackageManager.PERMISSION_GRANTED)" in policy
print("PASS framework enforces registered receiver exports against the sender UID")
