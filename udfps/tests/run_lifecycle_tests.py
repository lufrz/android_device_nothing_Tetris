#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real illumination app against a deterministic Android host fixture.

Only JNI, the native-library load and sysfs I/O are replaced in a temporary copy.
The request ownership, worker queue, display listener and timeout code stay intact.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

module = Path(__file__).resolve().parents[1]
root = next(p for p in module.parents if (p / "build/envsetup.sh").is_file())
jdk = Path(os.environ.get("JAVA_HOME", root / "prebuilts/jdk/jdk21/linux-x86"))
package = "org.lineageos.tetris.udfps"
app_path = module / "src/org/lineageos/tetris/udfps/IlluminationApplication.java"
app_source = Path(os.environ.get("TETRIS_UDFPS_TEST_APP_SOURCE", app_path))
app = app_source.read_text().replace('System.loadLibrary("tetris_udfps_surface");', '')
app, count = re.subn(
    r'    private static void writeNode\(String path, boolean enabled\) throws IOException \{.*?\n    \}',
    '    private static void writeNode(String path, boolean enabled) throws IOException {\n'
    '        TestHooks.write(path, enabled);\n    }', app, flags=re.S)
assert count == 1, "Update the sysfs test hook for the changed app"
app, count = re.subn(
    r'    private static void writeScanContext\(String command\) throws IOException \{.*?\n    \}',
    '    private static void writeScanContext(String command) throws IOException {\n'
    '        TestHooks.context(command);\n    }', app, flags=re.S)
assert count == 1, "Update the scan-context test hook for the changed app"
app = app.replace('Files.readString(Path.of(HBM))', 'TestHooks.readHbm()')
app = app.replace('Files.readString(Path.of(HBM_TIMING))', 'TestHooks.readHbmTiming()')
app, count = re.subn(
    r'    private static native boolean nativeShow\(.*?long generation\);',
    '    private static boolean nativeShow(int width, int height, int layerStack, float x,\n'
    '            float y, float radiusX, float radiusY, float alpha, long generation) {\n'
    '        if(generation!=TestHooks.generation)return false; TestHooks.shows++; TestHooks.events.add("show:"+TestHooks.contextMode); TestHooks.shownModes.add(TestHooks.contextMode); TestHooks.hbm=true; TestHooks.afterShow.run(); return true;\n    }', app, flags=re.S)
assert count == 1, "Update the JNI test hook for the changed app"
app = app.replace('private static native void nativeSetGeneration(long generation);',
                  'private static void nativeSetGeneration(long generation) { TestHooks.generation=generation; }')
app = app.replace('private static native boolean nativeHide();',
                  'private static boolean nativeHide() { TestHooks.hides++; TestHooks.events.add("hide"); TestHooks.hbm=false; return TestHooks.hideSucceeded; }')
app = app.replace('private static native String nativeGetDiagnostics();',
                  'private static String nativeGetDiagnostics() { return "host fixture"; }')
assert 'private static native ' not in app, "Unmocked JNI entry point"

stubs = {
"android/Manifest.java": '''package android; public final class Manifest {
 public static final class permission { public static final String DUMP="dump"; }}''',
"android/content/pm/PackageManager.java": '''package android.content.pm;
 public class PackageManager { public static final int PERMISSION_GRANTED=0; }''',
"android/content/res/Resources.java": '''package android.content.res;
 public class Resources { public int[] getIntArray(int id) { int[] a=new int[256];
 java.util.Arrays.fill(a,128); return a; } public int getInteger(int id) {
 return switch(id) { case 1 -> 256; case 2 -> 2680; case 3 -> 4095; default -> 4; }; }}''',
"android/app/Application.java": '''package android.app;
 public class Application { public static final java.util.Map<Class<?>,Object> services = new java.util.HashMap<>();
 public void onCreate() {} public <T> T getSystemService(Class<T> type) { return type.cast(services.get(type)); }
 public android.content.res.Resources getResources() { return new android.content.res.Resources(); }
 public int checkCallingOrSelfPermission(String permission) { return 0; }}''',
"android/graphics/Point.java": 'package android.graphics; public class Point { public int x,y; }',
"android/os/RemoteException.java": 'package android.os; public class RemoteException extends Exception {}',
"android/os/IBinder.java": '''package android.os; public interface IBinder {
 interface DeathRecipient { void binderDied(); }
 void linkToDeath(DeathRecipient recipient,int flags) throws RemoteException;
 boolean unlinkToDeath(DeathRecipient recipient,int flags); }''',
"android/os/Binder.java": '''package android.os; public class Binder implements IBinder {
 public static int getCallingUid() { return 1000; }
 public void linkToDeath(DeathRecipient recipient,int flags) throws RemoteException {}
 public boolean unlinkToDeath(DeathRecipient recipient,int flags) { return true; }
 public final void dumpForTest(java.io.PrintWriter out) { dump(null,out,new String[0]); }
 protected void dump(java.io.FileDescriptor fd,java.io.PrintWriter out,String[] args) {} }''',
"android/os/Process.java": 'package android.os; public class Process { public static final int SYSTEM_UID=1000; }',
"android/os/UserHandle.java": '''package android.os; public class UserHandle {
 public static final int USER_SYSTEM=0; public static int myUserId() { return 0; }}''',
"android/os/SystemClock.java": '''package android.os; public class SystemClock {
 public static long now; public static long uptimeMillis() { return now; }}''',
"android/os/HandlerThread.java": '''package android.os; public class HandlerThread {
 public HandlerThread(String name) {} public void start() {} public Object getLooper() { return this; }}''',
"android/os/Handler.java": '''package android.os; public class Handler {
 private record Task(long at,long order,Runnable task) implements Comparable<Task> {
 public int compareTo(Task t) { int c=Long.compare(at,t.at); return c!=0?c:Long.compare(order,t.order); }}
 private static final java.util.PriorityQueue<Task> queue=new java.util.PriorityQueue<>();
 private static long order; public Handler(Object looper) {}
 public boolean post(Runnable r) { return postAtTime(r,SystemClock.now); }
 public boolean postDelayed(Runnable r,long delay) { return postAtTime(r,SystemClock.now+delay); }
 public boolean postAtTime(Runnable r,long at) { queue.add(new Task(at,order++,r)); return true; }
 public static void reset() { queue.clear(); SystemClock.now=0; }
 public static void runDue() { int guard=1000; while(!queue.isEmpty()&&queue.peek().at<=SystemClock.now) {
 if(--guard==0) throw new AssertionError("worker failed to yield"); queue.remove().task.run(); }}
 public static void advance(long delta) { SystemClock.now+=delta; runDue(); }}''',
"android/os/ServiceManager.java": '''package android.os; public class ServiceManager {
 public static Object service; public static void addService(String n,Object s,boolean isolated) { service=s; }}''',
"android/os/PowerManager.java": '''package android.os; public class PowerManager {
 public static final int PARTIAL_WAKE_LOCK=1, WAKE_REASON_BIOMETRIC=17;
 public int wakes, interactiveQueries; public boolean interactive=true; public int failQueryAt=-1;
 public boolean isInteractive() { if(++interactiveQueries==failQueryAt)throw new IllegalStateException("power unavailable"); return interactive; }
 public final WakeLock lock=new WakeLock();
 public WakeLock newWakeLock(int level,String tag) { if(level!=PARTIAL_WAKE_LOCK) throw new AssertionError(); return lock; }
 public void wakeUp(long at,int reason,String details) { wakes++;
 throw new AssertionError("illumination must not replace the SystemUI Doze pulse with full wake"); }
 public static class WakeLock { public boolean held; public long deadline; public int releases;
 public void setReferenceCounted(boolean enabled) {}
 public void acquire(long timeout) { held=true; deadline=SystemClock.now+timeout; }
 public boolean isHeld() { return held&&SystemClock.now<deadline; }
 public void release() { held=false; releases++; }} }''',
"android/util/Log.java": '''package android.util; public class Log {
 public static int i(String t,String m) { return 0; } public static int e(String t,String m) { return 0; }
 public static int e(String t,String m,Throwable e) { return 0; }}''',
"android/hardware/display/BrightnessInfo.java": '''package android.hardware.display;
 public class BrightnessInfo { public float adjustedBrightness=.4f; }''',
"android/view/DisplayInfo.java": 'package android.view; public class DisplayInfo { public int state,committedState; }',
"android/view/Display.java": '''package android.view; public class Display {
 public static final int DEFAULT_DISPLAY=0,STATE_UNKNOWN=0,STATE_OFF=1,STATE_ON=2,STATE_DOZE=3,
 STATE_DOZE_SUSPEND=4,STATE_ON_SUSPEND=6; public int state=STATE_ON,committedState=STATE_ON;
 public boolean transitionToOffAtRead;
 public boolean getDisplayInfo(DisplayInfo info) { if(transitionToOffAtRead){state=STATE_OFF;transitionToOffAtRead=false;}
 info.state=state;info.committedState=committedState;return true; }
 public int getCommittedState() { return committedState; }
 public int getState() { int previous=state;if(transitionToOffAtRead){state=STATE_OFF;transitionToOffAtRead=false;}return previous; }
 public static String stateToString(int state) { return "state"+state; }
 public void getRealSize(android.graphics.Point p) { p.x=1080;p.y=2400; }
 public int getRotation() { return 0; } public int getLayerStack() { return 0; }
 public float getRefreshRate() { return 120; }
 public android.hardware.display.BrightnessInfo getBrightnessInfo() { return new android.hardware.display.BrightnessInfo(); }}''',
"android/hardware/display/DisplayManager.java": '''package android.hardware.display;
 public class DisplayManager { public final android.view.Display display=new android.view.Display();
 public static final long EVENT_TYPE_DISPLAY_ADDED=1,EVENT_TYPE_DISPLAY_CHANGED=4,
 EVENT_TYPE_DISPLAY_REMOVED=2,EVENT_TYPE_DISPLAY_STATE=16,EVENT_TYPE_DISPLAY_REFRESH_RATE=8,
 PRIVATE_EVENT_TYPE_DISPLAY_COMMITTED_STATE_CHANGED=4;
 public long publicEvents,privateEvents;public int committedOnlyCallbacks;
 private DisplayListener listener; private android.os.Handler handler;
 public interface DisplayListener { void onDisplayAdded(int id);void onDisplayRemoved(int id);void onDisplayChanged(int id); }
 public android.view.Display getDisplay(int id) { return display; } public float getBrightness(int id) { return .4f; }
 public void registerDisplayListener(DisplayListener l,android.os.Handler h) {
 registerDisplayListener(l,h,EVENT_TYPE_DISPLAY_ADDED|EVENT_TYPE_DISPLAY_CHANGED|EVENT_TYPE_DISPLAY_REMOVED,0); }
 public void registerDisplayListener(DisplayListener l,android.os.Handler h,long events,long privateFlags) {
 listener=l;handler=h;publicEvents=events;privateEvents=privateFlags; }
 public void request(int state) { display.state=state;
 if((publicEvents&EVENT_TYPE_DISPLAY_STATE)!=0)handler.post(()->listener.onDisplayChanged(0)); }
 public void commit(int state) { display.committedState=state;
 if((privateEvents&PRIVATE_EVENT_TYPE_DISPLAY_COMMITTED_STATE_CHANGED)!=0)
 handler.post(()->{committedOnlyCallbacks++;listener.onDisplayChanged(0);}); }
 public void change(int state) { display.state=state;display.committedState=state;
 if((publicEvents&EVENT_TYPE_DISPLAY_CHANGED)!=0)handler.post(()->listener.onDisplayChanged(0)); }}''',
"vendor/nothing/hardware/udfps/IIlluminationCallback.java": '''package vendor.nothing.hardware.udfps;
 public interface IIlluminationCallback { android.os.IBinder asBinder(); void onFailure() throws android.os.RemoteException; }''',
"vendor/nothing/hardware/udfps/IIllumination.java": '''package vendor.nothing.hardware.udfps;
 public interface IIllumination { String DESCRIPTOR="udfps",HASH="test"; int VERSION=1;
 int getInterfaceVersion();String getInterfaceHash();void begin(IIlluminationCallback client,int x,int y,int radius);
 void end(IIlluminationCallback client);abstract class Stub extends android.os.Binder implements IIllumination {} }''',
"org/lineageos/tetris/udfps/R.java": '''package org.lineageos.tetris.udfps; final class R {
 static final class array { static final int config_udfpsMtkGhbmAlphaMap=0; }
 static final class integer { static final int config_udfpsMtkGhbmAlphaMapScale=1,
 config_udfpsMtkGhbmNormalMaxBacklight=2,config_udfpsMtkGhbmMaxBacklight=3,config_udfpsMtkGhbmMinBacklight=4; }}''',
"org/lineageos/tetris/udfps/TestHooks.java": '''package org.lineageos.tetris.udfps;
 final class TestHooks { static int shows,hides;static boolean hbm,ui,hideSucceeded;
 static Runnable afterShow,afterContext;static int timingReads;static String timingMode;
 static String contextMode="none",contextFault="none";static long contextToken,generation;static int contextBegins,contextEnds;
 static boolean failReset;static final java.util.List<String> events=new java.util.ArrayList<>();
 static final java.util.List<String> shownModes=new java.util.ArrayList<>();
 static void context(String command) throws java.io.IOException {
  events.add(command);String[] fields=command.trim().split(" ");
  if(fields.length<3||!fields[0].equals("scan_v1"))throw new AssertionError("unexpected context protocol");
  long token=Long.parseLong(fields[2]);if(token<=0)throw new AssertionError("nonpositive token");
  if(fields[1].equals("begin")){
   contextBegins++;if(fields.length!=4||!java.util.Set.of("interactive","ambient").contains(fields[3]))throw new AssertionError();
   if(contextFault.equals("old"))throw new java.io.IOException("EINVAL");
   if(contextFault.equals("denied"))throw new java.nio.file.AccessDeniedException("hbm");
   if(hbm)throw new AssertionError("context begin while HBM already on");
   contextToken=token;contextMode=fields[3];afterContext.run();
   if(contextFault.equals("close"))throw new java.io.IOException("close failed after accepted write");
  }else if(fields[1].equals("end")){
   contextEnds++;if(fields.length!=3)throw new AssertionError();
   if(contextFault.equals("old"))throw new java.io.IOException("EINVAL");
   if(contextToken==token){contextToken=0;contextMode="none";}
  }else throw new AssertionError("unexpected operation");
 }
 static void reset() { shows=hides=timingReads=0;hbm=ui=false;hideSucceeded=true;
  afterShow=afterContext=()->{};timingMode="missing";contextMode="none";contextFault="none";
  contextToken=0;contextBegins=contextEnds=0;failReset=false;events.clear();shownModes.clear(); }
 static String readHbmTiming() throws java.io.IOException { timingReads++;
  if(timingMode.equals("missing"))throw new java.nio.file.NoSuchFileException("hbm_timing");
  if(timingMode.equals("denied"))throw new java.nio.file.AccessDeniedException("hbm_timing");
  if(timingMode.equals("security"))throw new SecurityException("hbm_timing");
  return timingMode; }
 static void write(String path,boolean on) throws java.io.IOException {
  if(path.endsWith("/hbm")){events.add(on?"hbm1":"hbm0");if(on)throw new AssertionError("context must never force HBM on");
   if(failReset)throw new java.io.IOException("reset failed");hbm=false;contextToken=0;contextMode="none";
  }else ui=on; }
 static String readHbm() { return hbm?"1":"0"; }}''',
}

harness = '''package org.lineageos.tetris.udfps;
import android.app.Application;import android.os.*;import android.view.Display;
import android.hardware.display.DisplayManager;import vendor.nothing.hardware.udfps.*;
public final class DisplayLifecycleTest {
 static PowerManager power;static DisplayManager displays;static IIllumination service;
 static class Client extends Binder implements IIlluminationCallback {
  int failures,links;IBinder.DeathRecipient death;public IBinder asBinder(){return this;}public void onFailure(){failures++;}
  @Override public void linkToDeath(IBinder.DeathRecipient recipient,int flags){links++;death=recipient;}
  @Override public boolean unlinkToDeath(IBinder.DeathRecipient recipient,int flags){if(death==recipient)death=null;return true;}
  void die(){IBinder.DeathRecipient d=death;if(d!=null)d.binderDied();}}
 static void check(boolean ok,String message){if(!ok)throw new AssertionError(message);}
 static Client setup(int state){return setup(state,true);}
 static Client setup(int state,boolean powerAvailable){
  Handler.reset();TestHooks.reset();ServiceManager.service=null;service=null;power=new PowerManager();
  power.interactive=state==Display.STATE_ON;
  displays=new DisplayManager();displays.display.state=state;displays.display.committedState=state;
  Application.services.clear();
  if(powerAvailable)Application.services.put(PowerManager.class,power);
  Application.services.put(DisplayManager.class,displays);
  new IlluminationApplication().onCreate();Handler.runDue();service=(IIllumination)ServiceManager.service;
  check(service!=null,"startup must register a fresh illumination service");
  return new Client();}
 static void begin(Client client){service.begin(client,540,2109,93);}
 static String dump(){java.io.StringWriter text=new java.io.StringWriter();
  ((Binder)service).dumpForTest(new java.io.PrintWriter(text));return text.toString();}
 static String ownerGeneration(){
  String diagnosis=dump();int start=diagnosis.indexOf("generation=");
  int end=diagnosis.indexOf(" owner=true",start);
  check(start>=0&&end>start,"expected an active owner in diagnostic dump");
  return diagnosis.substring(start,end);}
 static void missingPowerManager(int state){
  Client failed=setup(state,false);IIllumination registered=service;
  check(TestHooks.shows==0&&!TestHooks.hbm&&!TestHooks.ui,
    "startup without PowerManager must not illuminate");
  check(dump().contains("powerManager=false"),"missing power dependency must be inspectable");
  begin(failed);Handler.runDue();
  check(failed.failures==1&&TestHooks.shows==0&&!TestHooks.hbm&&!TestHooks.ui
    &&power.wakes==0&&!power.lock.isHeld(),
    "missing PowerManager must fail the contact once without wake or illumination");
  check(ServiceManager.service==registered&&service.getInterfaceVersion()==IIllumination.VERSION,
    "dependency failure must preserve the registered Binder service");
  String diagnosis=dump();
  check(diagnosis.contains("PowerManager is unavailable (power/thermalservice)")
    &&diagnosis.contains("owner=false")&&diagnosis.contains("scanWakeLock=false"),
    "dump must explain the missing dependency and released ownership");
  Handler.advance(10000);
  check(failed.failures==1&&TestHooks.shows==0,"failed contact timeout must remain inert");
  Application.services.put(PowerManager.class,power);
  Client retry=new Client();begin(retry);Handler.runDue();
  if(state==Display.STATE_OFF){
   check(power.wakes==0&&TestHooks.shows==0&&power.lock.isHeld(),
     "restored power dependency must wait for an external pulse with CPU held");
   String generation=ownerGeneration();Handler.advance(500);
   displays.change(Display.STATE_ON);Handler.runDue();
   check(ownerGeneration().equals(generation)&&retry.links==1,
     "power recovery must render the same request when SystemUI pulses");
  }
  Handler.advance(17);
  check(retry.failures==0&&TestHooks.shows==1&&TestHooks.hbm&&TestHooks.ui&&power.lock.isHeld()
    &&ServiceManager.service==registered,"a new contact must recover without restarting the service");
  check(dump().contains("powerManager=true"),"dump must report recovered power dependency");
  service.end(retry);Handler.runDue();
  check(!power.lock.isHeld()&&!TestHooks.hbm&&!TestHooks.ui,"recovered contact must clean up");
 }
 static void dozeScanGate(){
  Client c=setup(Display.STATE_DOZE);begin(c);Handler.runDue();String generation=ownerGeneration();
  check(TestHooks.shows==0&&!TestHooks.hbm&&!TestHooks.ui&&c.failures==0&&power.lock.isHeld(),
    "DOZE must wait for ON without rendering, HBM or UI-ready");
  check(dump().contains("wait_from=state3"),"DOZE wait must be visible in diagnostics");
  displays.change(Display.STATE_DOZE);Handler.runDue();Handler.advance(500);
  check(TestHooks.shows==0&&c.failures==0&&ownerGeneration().equals(generation),
    "repeated DOZE events must keep the same pending contact");
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
  check(TestHooks.shows==1&&TestHooks.hbm&&TestHooks.ui&&c.failures==0&&c.links==1
    &&ownerGeneration().equals(generation)&&power.wakes==0,
    "DOZE to ON must render and authorize the same contact once");
  displays.change(Display.STATE_DOZE);Handler.runDue();
  check(c.failures==1&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld()
    &&dump().contains("owner=false"),"ON to DOZE must revoke an active scan immediately");
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(10000);
  check(TestHooks.shows==1&&c.failures==1,"later ON cannot restart a scan aborted by DOZE");
  c=setup(Display.STATE_DOZE);begin(c);Handler.runDue();service.end(c);
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(2000);
  check(c.failures==0&&TestHooks.shows==0&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),
    "lift while waiting in DOZE must cancel the late ON pulse");
  c=setup(Display.STATE_ON);TestHooks.afterShow=()->displays.display.state=Display.STATE_DOZE;
  begin(c);Handler.runDue();Handler.advance(17);
  check(c.failures==1&&TestHooks.shows==1&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),
    "DOZE reached during presentation must not authorize the sensor");
  c=setup(Display.STATE_ON);begin(c);Handler.runDue();
  displays.display.state=Display.STATE_DOZE;Handler.advance(17);
  check(c.failures==1&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),
    "ready must recheck ON even before a queued display listener runs");
  c=setup(Display.STATE_ON);begin(c);Handler.runDue();Handler.advance(17);
  TestHooks.hideSucceeded=false;displays.change(Display.STATE_DOZE);Handler.runDue();
  check(c.failures==1&&dump().contains("state=hardware reset failed")&&!power.lock.isHeld(),
    "failed surface removal in drawable DOZE must remain a cleanup error");
  System.out.println("PASS: DOZE waits for ON, same-contact continuity, lift cancellation, presentation/readiness rechecks and scan cleanup");
 }
 static void committedDisplayGate(){
  Client c=setup(Display.STATE_ON);displays.display.committedState=Display.STATE_OFF;
  begin(c);Handler.runDue();String generation=ownerGeneration();
  check(TestHooks.shows==0&&!TestHooks.hbm&&!TestHooks.ui&&c.failures==0&&power.lock.isHeld(),
    "requested ON with committed OFF must wait without rendering, HBM or UI-ready");
  check(dump().contains("display=state2")&&dump().contains("displayCommitted=state1")
    &&dump().contains("wait_from=state2 wait_committed_from=state1")
    &&dump().contains("requested_on_observed_elapsed_ms=0")
    &&!dump().contains("committed_on_observed_elapsed_ms="),
    "diagnostics must distinguish requested ON from uncommitted display power");
  displays.request(Display.STATE_ON);Handler.runDue();Handler.advance(123);
  check(TestHooks.shows==0&&c.failures==0&&ownerGeneration().equals(generation),
    "another requested ON event cannot bypass the committed-state gate");
  displays.commit(Display.STATE_ON);Handler.runDue();
  check(displays.committedOnlyCallbacks==1&&TestHooks.shows==1&&TestHooks.hbm&&!TestHooks.ui
    &&c.failures==0&&c.links==1&&ownerGeneration().equals(generation),
    "committed-only event must render the same contact without a basic display-changed event");
  check(dump().contains("displayCommitted=state2")
    &&dump().contains("committed_on_observed_elapsed_ms=123")
    &&dump().contains("display=state2 display_committed=state2"),
    "completion diagnostics must record when committed ON was first observed");
  Handler.advance(17);check(TestHooks.ui,"committed ON still waits for panel settling before UI-ready");
  displays.commit(Display.STATE_ON);Handler.runDue();Handler.advance(2000);
  check(TestHooks.shows==1&&c.failures==0&&power.lock.isHeld(),
    "duplicate completion and old wait timeout must not recreate or abort the contact");
  service.end(c);Handler.runDue();
  check(!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),"completed contact must clean up normally");
  c=setup(Display.STATE_ON);displays.display.committedState=Display.STATE_OFF;
  begin(c);Handler.runDue();service.end(c);displays.commit(Display.STATE_ON);Handler.runDue();
  Handler.advance(10000);
  check(c.failures==0&&TestHooks.shows==0&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),
    "late committed ON after lift must not illuminate or report failure");
  c=setup(Display.STATE_ON);displays.display.committedState=Display.STATE_OFF;
  begin(c);Handler.runDue();Handler.advance(1999);
  check(c.failures==0&&TestHooks.shows==0&&power.lock.isHeld(),
    "pending committed power transition must retain the contact until its existing deadline");
  Handler.advance(1);
  check(c.failures==1&&TestHooks.shows==0&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld()
    &&dump().contains("owner=false"),"uncommitted ON must time out once and release its owner");
  displays.commit(Display.STATE_ON);Handler.runDue();Handler.advance(10000);
  check(c.failures==1&&TestHooks.shows==0,"late committed ON must not revive a timed-out owner");
  c=setup(Display.STATE_ON);TestHooks.afterShow=()->displays.display.committedState=Display.STATE_OFF;
  begin(c);Handler.runDue();Handler.advance(17);
  check(c.failures==1&&TestHooks.shows==1&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),
    "loss of committed ON during presentation must prevent sensor readiness");
  c=setup(Display.STATE_ON);begin(c);Handler.runDue();
  displays.display.committedState=Display.STATE_OFF;Handler.advance(17);
  check(c.failures==1&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),
    "ready must recheck committed ON before a queued display listener can run");
  c=setup(Display.STATE_ON);begin(c);Handler.runDue();Handler.advance(17);
  displays.commit(Display.STATE_OFF);Handler.runDue();
  check(c.failures==1&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),
    "committed-only power loss must revoke an active scan even if requested state is still ON");
  c=setup(Display.STATE_ON);displays.display.transitionToOffAtRead=true;
  try {
   java.lang.reflect.Method gate=IlluminationApplication.class.getDeclaredMethod("isScanReady",Display.class);
   gate.setAccessible(true);
   check(!((Boolean)gate.invoke(null,displays.display)),
     "display readiness must use one coherent snapshot when power-off starts during a read");
  } catch(ReflectiveOperationException e){throw new AssertionError(e);}
  check(displays.display.state==Display.STATE_OFF&&displays.display.committedState==Display.STATE_ON,
    "snapshot race fixture must preserve a pending OFF transition with previous committed ON");
  System.out.println("PASS: requested/committed ON gate, coherent snapshot, private completion event, same-generation resume, cancellation, timeout and readiness rechecks");
 }
 static void optionalHbmTiming(){
  Client c=setup(Display.STATE_OFF);begin(c);Handler.runDue();
  check(TestHooks.timingReads==0,"startup and waiting must not read optional HBM timing");
  check(dump().contains("hbmTiming=unavailable (NoSuchFileException)"),
    "kernel without the optional node must remain diagnosable while awaiting its pulse");
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
  check(c.failures==0&&TestHooks.shows==1&&TestHooks.hbm&&TestHooks.ui&&power.lock.isHeld(),
    "missing optional node must not prevent a same-contact OFF-to-ON capture");
  final int readsBefore=TestHooks.timingReads;
  for(String mode:new String[]{"missing","denied","security","presented=42",""}){
   TestHooks.timingMode=mode;
   final int shows=TestHooks.shows,hides=TestHooks.hides;
   String diagnosis=dump();
   if(mode.equals("missing"))check(diagnosis.contains("hbmTiming=unavailable (NoSuchFileException)"),
      "missing node must be reported without failing the scan");
   else if(mode.equals("denied"))check(diagnosis.contains("hbmTiming=unavailable (AccessDeniedException)"),
      "read denial must be reported without failing the scan");
   else if(mode.equals("security"))check(diagnosis.contains("hbmTiming=unavailable (SecurityException)"),
      "security denial must be reported without failing the scan");
   else check(diagnosis.contains("hbmTiming:\\n"+mode),"optional timing content must be printed unchanged");
   check(c.failures==0&&TestHooks.shows==shows&&TestHooks.hides==hides&&TestHooks.hbm
      &&TestHooks.ui&&power.lock.isHeld()&&diagnosis.contains("state=illuminating"),
      "optional dump reads must not change an active capture");
  }
  check(TestHooks.timingReads==readsBefore+5,"only dumpsys must read optional timing");
  final int readsAtEnd=TestHooks.timingReads;
  service.end(c);Handler.runDue();
  check(TestHooks.timingReads==readsAtEnd&&!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),
    "cleanup must not depend on the optional diagnostic node");
  System.out.println("PASS: optional HBM timing absent, denied, readable or empty never gates capture or cleanup");
 }
 static void scanContextLifecycle(){
  Client c=setup(Display.STATE_ON);begin(c);Handler.runDue();Handler.advance(17);
  check(c.failures==0&&TestHooks.contextBegins==1&&TestHooks.shownModes.equals(java.util.List.of("interactive")),
    "already interactive ON capture must publish exactly one interactive token before rendering");
  long first=TestHooks.contextToken;check(first>0,"scan token must be positive");
  begin(c);Handler.runDue();check(TestHooks.contextBegins==1,"duplicate owner begin must not renew context");
  service.end(c);Handler.runDue();
  int hide=TestHooks.events.lastIndexOf("hide"),off=TestHooks.events.lastIndexOf("hbm0");
  int end=TestHooks.events.lastIndexOf("scan_v1 end "+first);
  check(hide<off&&off<end&&TestHooks.contextToken==0&&!TestHooks.hbm,
    "end token must follow presented hide and HBM0 cleanup");
  c=setup(Display.STATE_ON);power.interactive=false;begin(c);Handler.runDue();
  check(TestHooks.shownModes.equals(java.util.List.of("ambient")),
    "a display already ON during a noninteractive AOD pulse must remain ambient");
  power.interactive=true;service.end(c);Handler.runDue();
  check(TestHooks.contextBegins==1&&TestHooks.contextToken==0,"unlock before OFF must not reclassify the token");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();power.interactive=true;
  displays.change(Display.STATE_ON);Handler.runDue();
  check(TestHooks.shownModes.equals(java.util.List.of("ambient")),
    "noninteractive origin must never upgrade after display wake");
  service.end(c);Handler.runDue();
  c=setup(Display.STATE_ON);displays.display.committedState=Display.STATE_OFF;
  begin(c);Handler.runDue();power.interactive=false;displays.commit(Display.STATE_ON);Handler.runDue();
  check(TestHooks.shownModes.equals(java.util.List.of("ambient")),
    "interactive origin must downgrade if interactivity is lost before rendering");
  service.end(c);Handler.runDue();
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();service.end(c);
  displays.change(Display.STATE_ON);Handler.runDue();
  check(TestHooks.contextBegins==0&&TestHooks.shows==0,"cancellation during display wait must not publish context");
  c=setup(Display.STATE_ON);begin(c);Client newer=new Client();begin(newer);Handler.runDue();
  check(TestHooks.contextBegins==1&&TestHooks.shows==1,"replacement before worker must skip stale publication");
  long currentToken=TestHooks.contextToken;service.end(c);Handler.runDue();
  check(TestHooks.contextToken==currentToken&&TestHooks.hbm,"stale end must not revoke new owner context");
  service.end(newer);Handler.runDue();
  c=setup(Display.STATE_ON);begin(c);Handler.runDue();first=TestHooks.contextToken;
  newer=new Client();begin(newer);Handler.runDue();currentToken=TestHooks.contextToken;
  check(first!=currentToken&&currentToken>0&&TestHooks.contextBegins==2&&TestHooks.contextEnds>=1,
    "replacement of an active capture must clear old token before publishing a new one");
  check(TestHooks.events.indexOf("scan_v1 end "+first)<TestHooks.events.indexOf("scan_v1 begin "+currentToken+" interactive"),
    "old context must not survive across new context publication");
  c.die();service.end(c);Handler.runDue();check(TestHooks.contextToken==currentToken,"stale owner death/end must be inert");
  newer.die();Handler.runDue();check(TestHooks.contextToken==0&&!TestHooks.hbm&&!power.lock.isHeld(),
    "active owner death must clear context and illumination");
  c=setup(Display.STATE_ON);final Client cancelled=c;
  TestHooks.afterContext=()->service.end(cancelled);begin(c);Handler.runDue();
  check(TestHooks.shows==0&&TestHooks.contextToken==0&&!TestHooks.hbm,
    "cancellation after accepted context write must not render stale generation or leak token");
  for(String fault:new String[]{"old","denied","close"}){
   c=setup(Display.STATE_ON);TestHooks.contextFault=fault;begin(c);Handler.runDue();Handler.advance(17);
   check(c.failures==0&&TestHooks.contextBegins==1&&TestHooks.shownModes.equals(java.util.List.of("none"))
     &&TestHooks.contextToken==0&&TestHooks.ui,"unsupported, denied or ambiguously accepted context must reset before baseline capture: "+fault);
   int publish=-1;for(int i=0;i<TestHooks.events.size();i++)if(TestHooks.events.get(i).startsWith("scan_v1 begin "))publish=i;
   int reset=TestHooks.events.subList(publish+1,TestHooks.events.size()).indexOf("hbm0")+publish+1;
   check(publish<reset&&reset<TestHooks.events.indexOf("show:none"),"fallback reset must precede rendering: "+fault);
   displays.change(Display.STATE_ON);Handler.runDue();check(TestHooks.contextBegins==1,"failed context may not retry during same scan");
   service.end(c);Handler.runDue();check(!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),"fallback capture must clean up");
  }
  c=setup(Display.STATE_ON);TestHooks.contextFault="close";TestHooks.afterContext=()->TestHooks.failReset=true;
  begin(c);Handler.runDue();check(c.failures==1&&TestHooks.shows==0&&!TestHooks.ui,
    "failed reset after ambiguous accepted context must abort instead of rendering");
  for(int failedQuery:new int[]{1,2}){
   c=setup(Display.STATE_ON);power.failQueryAt=failedQuery;begin(c);Handler.runDue();
   check(!TestHooks.shownModes.contains("interactive"),"unavailable interactivity must fail closed");
   service.end(c);Handler.runDue();
  }
  System.out.println("PASS: scan context interactive/AOD origin, no upgrade, downgrade, cancellation, replacement, death, old-kernel fallback and ambiguous-write reset");
 }
 public static void main(String[] args){
  scanContextLifecycle();
  optionalHbmTiming();
  committedDisplayGate();
  dozeScanGate();
  missingPowerManager(Display.STATE_ON);missingPowerManager(Display.STATE_OFF);
  System.out.println("PASS: missing PowerManager startup, request failure, diagnostic dump and same-process recovery");
  Client c=setup(Display.STATE_ON);displays.change(Display.STATE_OFF);begin(c);Handler.runDue();
  check(c.failures==0&&power.wakes==0&&TestHooks.shows==0&&power.lock.isHeld(),
    "queued OFF before start must retain the contact while awaiting a pulse");
  service.end(c);Handler.runDue();
  c=setup(Display.STATE_ON);begin(c);Handler.runDue();
  displays.change(Display.STATE_OFF);service.end(c);Client replacement=new Client();begin(replacement);Handler.runDue();
  check(replacement.failures==0&&power.wakes==0&&power.lock.isHeld()&&TestHooks.shows==1,
    "old geometry and queued OFF must not abort replacement waiting for display");
  service.end(replacement);Handler.runDue();
  c=setup(Display.STATE_OFF);begin(c);service.end(c);Handler.runDue();
  check(power.wakes==0&&TestHooks.shows==0,"cancel before worker must not wake or render");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();
  check(power.wakes==0,"OFF must never request full wake");
  check(power.lock.isHeld()&&power.lock.deadline==12000,"bounded CPU wake lock");
  check(TestHooks.shows==0&&!TestHooks.hbm,"OFF must wait without rendering or HBM");
  service.end(c);displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(2000);
  check(TestHooks.shows==0&&c.failures==0&&!power.lock.isHeld(),"late ON after lift must stay cancelled");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();
  String heldGeneration=ownerGeneration();
  check(dump().contains("wait_from=state1")&&dump().contains("display_wait_started_uptime_ms=0")
    &&dump().contains("displayWake=systemui-doze"),"waiting diagnostics must identify the SystemUI pulse owner");
  Handler.advance(499);
  check(c.failures==0&&c.links==1&&TestHooks.shows==0&&!TestHooks.ui&&power.lock.isHeld()
    &&ownerGeneration().equals(heldGeneration),"held contact must survive asynchronous proximity checking");
  begin(c);Handler.runDue();
  check(c.links==1&&ownerGeneration().equals(heldGeneration),"duplicate begin must not replace the pending request");
  Handler.advance(1);
  displays.change(Display.STATE_ON);displays.change(Display.STATE_ON);Handler.runDue();
  check(TestHooks.shows==1&&TestHooks.hbm&&c.links==1&&c.failures==0
    &&ownerGeneration().equals(heldGeneration)&&power.wakes==0,
    "SystemUI pulse after 500ms must present the same held contact once, without a second touch");
  Handler.advance(17);check(TestHooks.ui,"UI ready follows presentation and panel wait");
  Handler.advance(1483);
  check(c.failures==0&&TestHooks.ui&&ownerGeneration().equals(heldGeneration),
    "expired display wait timer must not abort a request that already presented");
  service.end(c);Handler.runDue();
  check(!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),"end clears hardware and CPU lock");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();Handler.advance(1999);
  check(c.failures==0&&power.lock.isHeld(),"display wait must retain the contact until its deadline");
  Handler.advance(1);
  check(c.failures==1&&TestHooks.shows==0&&!power.lock.isHeld()&&!TestHooks.hbm&&!TestHooks.ui
    &&dump().contains("owner=false"),"display timeout must clean hardware and release ownership exactly once");
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(10000);
  check(c.failures==1&&TestHooks.shows==0,"late ON/scan timeout cannot restart failed request");
  for(int state:new int[]{Display.STATE_DOZE_SUSPEND,Display.STATE_ON_SUSPEND,Display.STATE_UNKNOWN}){
   c=setup(state);begin(c);Handler.runDue();String generation=ownerGeneration();
   check(power.wakes==0&&TestHooks.shows==0&&power.lock.isHeld(),"suspended state must wait without full wake");
   displays.change(state);Handler.runDue();Handler.advance(500);
   check(TestHooks.shows==0&&c.failures==0,"repeated suspended state must not start or cancel presentation");
   displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
   check(TestHooks.shows==1&&TestHooks.ui&&c.failures==0&&c.links==1
     &&ownerGeneration().equals(generation),"external pulse must start the suspended request without another contact");
   service.end(c);Handler.runDue();}
  for(int state:new int[]{Display.STATE_ON}){
   c=setup(state);begin(c);Handler.runDue();check(power.wakes==0&&TestHooks.shows==1,"ON starts without full wake");
   service.end(c);Handler.runDue();}
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();Handler.advance(1000);service.end(c);
  Client newer=new Client();begin(newer);Handler.runDue();Handler.advance(1000);
  check(newer.failures==0&&power.lock.isHeld(),"old display timeout cannot cancel newer owner");
  displays.change(Display.STATE_ON);Handler.runDue();check(TestHooks.shows==1,"newer owner renders");
  displays.change(Display.STATE_OFF);Handler.runDue();
  check(newer.failures==1&&!power.lock.isHeld()&&power.wakes==0,"power-off during scan aborts without full wake");
  System.out.println("PASS: SystemUI-only display pulse, 500ms held-contact continuity, lift cancellation, timeout ownership, DOZE and bounded cleanup");
 }
}'''

with tempfile.TemporaryDirectory(prefix="tetris-udfps-lifecycle-") as temporary:
    temp = Path(temporary)
    for relative, text in stubs.items():
        target = temp / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    src = temp / package.replace('.', '/')
    (src / 'IlluminationApplication.java').write_text(app)
    (src / 'DisplayLifecycleTest.java').write_text(harness)
    for name in ['Calibration.java', 'SensorGeometry.java']:
        (src / name).write_text((app_path.parent / name).read_text())
    classes = temp / 'classes'
    subprocess.run([str(jdk / 'bin/javac'), '-d', str(classes),
                    *map(str, temp.rglob('*.java'))], check=True)
    subprocess.run([str(jdk / 'bin/java'), '-cp', str(classes),
                    package + '.DisplayLifecycleTest'], check=True)
