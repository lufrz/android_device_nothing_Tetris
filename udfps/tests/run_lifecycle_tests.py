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
    '        if(generation!=TestHooks.generation)return false; TestHooks.shows++; TestHooks.onShow(width,height,x,y,radiusX,radiusY,alpha); TestHooks.events.add("show:"+TestHooks.contextMode); TestHooks.shownModes.add(TestHooks.contextMode); TestHooks.hbm=true; TestHooks.afterShow.run(); return true;\n    }', app, flags=re.S)
assert count == 1, "Update the JNI test hook for the changed app"
app = app.replace('private static native void nativeSetGeneration(long generation);',
                  'private static void nativeSetGeneration(long generation) { TestHooks.generation=generation; TestHooks.cancelPreparation(); }')
app = app.replace('private static native boolean nativeHide();',
                  'private static boolean nativeHide() { TestHooks.hides++; TestHooks.events.add("hide"); TestHooks.hbm=false; TestHooks.afterHide.run(); return TestHooks.hideSucceeded; }')
app = app.replace('private static native String nativeGetDiagnostics();',
                  'private static String nativeGetDiagnostics() { return "host fixture"; }')
app = app.replace('private static native long nativeCancelPreparation();',
                  'private static long nativeCancelPreparation() { return TestHooks.cancelPreparation(); }')
app, count_has = re.subn(
    r'    private static native boolean nativeHasBuffer\(.*?\);',
    '    private static boolean nativeHasBuffer(int width, int height, float x, float y,\n'
    '            float radiusX, float radiusY, float alpha) {\n'
    '        return TestHooks.hasBuffer(width,height,x,y,radiusX,radiusY,alpha);\n    }', app, flags=re.S)
app, count_prepare = re.subn(
    r'    private static native boolean nativePrepareBuffer\(.*?\);',
    '    private static boolean nativePrepareBuffer(int width, int height, float x, float y,\n'
    '            float radiusX, float radiusY, float alpha, long token) {\n'
    '        return TestHooks.prepare(width,height,x,y,radiusX,radiusY,alpha,token);\n    }', app, flags=re.S)
assert count_has <= 1 and count_prepare <= 1, "Duplicate preparation entry point"
assert 'private static native ' not in app, "Unmocked JNI entry point"

stubs = {
"android/Manifest.java": '''package android; public final class Manifest {
 public static final class permission { public static final String DUMP="dump"; }}''',
"android/content/Intent.java": '''package android.content; public class Intent {
 public static final int FLAG_RECEIVER_REGISTERED_ONLY=0x40000000,FLAG_RECEIVER_FOREGROUND=0x10000000;
 private final String action;private String targetPackage;private int flags;
 public Intent(String action){this.action=action;}
 public String getAction(){return action;}public Intent setPackage(String value){targetPackage=value;return this;}
 public String getPackage(){return targetPackage;}public Intent addFlags(int value){flags|=value;return this;}
 public Intent setFlags(int value){flags=value;return this;}public int getFlags(){return flags;}}
''',
"android/content/pm/PackageManager.java": '''package android.content.pm;
 public class PackageManager { public static final int PERMISSION_GRANTED=0; }''',
"android/content/res/Resources.java": '''package android.content.res;
 public class Resources { public int[] getIntArray(int id) { int[] a=new int[256];
 for(int i=0;i<a.length;i++)a[i]=i; return a; } public int getInteger(int id) {
 return switch(id) { case 1 -> 256; case 2 -> 2680; case 3 -> 4095; default -> 4; }; }}''',
"android/app/Application.java": '''package android.app;
 public class Application { public static final java.util.Map<Class<?>,Object> services = new java.util.HashMap<>();
 public record Broadcast(android.content.Intent intent,android.os.UserHandle user) {}
 public static final java.util.List<Broadcast> broadcasts=new java.util.ArrayList<>();
 public static RuntimeException broadcastFailure,executorFailure;
 public static final java.util.ArrayDeque<Runnable> mainQueue=new java.util.ArrayDeque<>();
 public java.util.concurrent.Executor getMainExecutor(){return task->{if(executorFailure!=null)throw executorFailure;mainQueue.add(task);};}
 public static boolean runOneMain(){Runnable task=mainQueue.poll();if(task==null)return false;task.run();return true;}
 public static void runMain(){int guard=100;while(runOneMain())if(--guard==0)throw new AssertionError("main executor loop");}
 public static java.util.function.Consumer<android.content.Intent> beforeBroadcast=ignored->{};
 public static Runnable afterBroadcast=()->{};
 public void sendBroadcastAsUser(android.content.Intent intent,android.os.UserHandle user){
  broadcasts.add(new Broadcast(intent,user));beforeBroadcast.accept(intent);
  if(broadcastFailure!=null)throw broadcastFailure;afterBroadcast.run();}
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
"android/os/Process.java": 'package android.os; public class Process { public static final int SYSTEM_UID=1000,THREAD_PRIORITY_BACKGROUND=10; }',
"android/os/UserHandle.java": '''package android.os; public class UserHandle {
 public static final int USER_SYSTEM=0;
 public static final UserHandle SYSTEM=new UserHandle(0),CURRENT=new UserHandle(-2);
 private final int identifier;public UserHandle(int id){identifier=id;}public int getIdentifier(){return identifier;}
 public static int myUserId() { return 0; }}''',
"android/os/SystemClock.java": '''package android.os; public class SystemClock {
 public static long now; public static long uptimeMillis() { return now; }}''',
"android/os/HandlerThread.java": '''package android.os; public class HandlerThread {
 public final String name;public final int priority;
 public HandlerThread(String name) { this(name,0); } public HandlerThread(String name,int priority) {this.name=name;this.priority=priority;}
 public void start() {} public Object getLooper() { return this; }}''',
"android/os/Handler.java": '''package android.os; public class Handler {
 private record Task(long at,long order,Handler owner,Runnable task) implements Comparable<Task> {
 public int compareTo(Task t) { int c=Long.compare(at,t.at); return c!=0?c:Long.compare(order,t.order); }}
 private static final java.util.PriorityQueue<Task> queue=new java.util.PriorityQueue<>();
 private static long order;private final boolean background;public static boolean inBackground;
 public Handler(Object looper) { background=looper instanceof HandlerThread t&&t.priority==Process.THREAD_PRIORITY_BACKGROUND; }
 public boolean post(Runnable r) { return postAtTime(r,SystemClock.now); }
 public boolean postDelayed(Runnable r,long delay) { return postAtTime(r,SystemClock.now+delay); }
 public boolean postAtTime(Runnable r,long at) { queue.add(new Task(at,order++,this,r)); return true; }
 public void removeCallbacks(Runnable r) { queue.removeIf(t->t.owner==this&&t.task==r); }
 public static void reset() { queue.clear();SystemClock.now=0;order=0;inBackground=false; }
 private static Task next(boolean background,boolean dueOnly) { return queue.stream()
  .filter(t->t.owner.background==background&&(!dueOnly||t.at<=SystemClock.now)).min(Task::compareTo).orElse(null); }
 public static boolean runOne(boolean background) { Task t=next(background,true);if(t==null)return false;
  queue.remove(t);boolean previous=inBackground;inBackground=background;try{t.task.run();}finally{inBackground=previous;}return true; }
 public static void runDue() { int guard=1000;while(runOne(false)) {
  if(--guard==0)throw new AssertionError("worker failed to yield"); }}
 public static boolean runOneBackground() { return runOne(true); }
 public static int backgroundPending() { return (int)queue.stream().filter(t->t.owner.background).count(); }
 public static int foregroundPending() { return (int)queue.stream().filter(t->!t.owner.background).count(); }
 public static void advance(long delta) { SystemClock.now+=delta;runDue(); }}''',
"android/os/ServiceManager.java": '''package android.os; public class ServiceManager {
 public static Object service; public static void addService(String n,Object s,boolean isolated) { service=s; }}''',
"android/os/PowerManager.java": '''package android.os; public class PowerManager {
 public static final int PARTIAL_WAKE_LOCK=1, WAKE_REASON_BIOMETRIC=17;
 public int wakes, interactiveQueries; public boolean interactive=true; public int failQueryAt=-1;
 public Runnable beforeInteractive=()->{};
 public boolean isInteractive() { beforeInteractive.run();if(++interactiveQueries==failQueryAt)throw new IllegalStateException("power unavailable"); return interactive; }
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
 public static int e(String t,String m,Throwable e) { return 0; }
 public static int w(String t,String m,Throwable e) { return 0; }}''',
"android/hardware/display/BrightnessInfo.java": '''package android.hardware.display;
 public class BrightnessInfo { public float adjustedBrightness=.4f; }''',
"android/view/DisplayInfo.java": 'package android.view; public class DisplayInfo { public int state,committedState; }',
"android/view/Display.java": '''package android.view; public class Display {
 public static final int DEFAULT_DISPLAY=0,STATE_UNKNOWN=0,STATE_OFF=1,STATE_ON=2,STATE_DOZE=3,
 STATE_DOZE_SUSPEND=4,STATE_ON_SUSPEND=6; public int state=STATE_ON,committedState=STATE_ON;
 public boolean transitionToOffAtRead;public int width=1080,height=2400,rotation;
 public float brightness=.4f;public boolean missingBrightnessInfo;
 public boolean getDisplayInfo(DisplayInfo info) { if(transitionToOffAtRead){state=STATE_OFF;transitionToOffAtRead=false;}
 info.state=state;info.committedState=committedState;return true; }
 public int getCommittedState() { return committedState; }
 public int getState() { int previous=state;if(transitionToOffAtRead){state=STATE_OFF;transitionToOffAtRead=false;}return previous; }
 public static String stateToString(int state) { return "state"+state; }
 public void getRealSize(android.graphics.Point p) { p.x=width;p.y=height; }
 public int getRotation() { return rotation; } public int getLayerStack() { return 0; }
 public float getRefreshRate() { return 120; }
 public android.hardware.display.BrightnessInfo getBrightnessInfo() { if(missingBrightnessInfo)return null;var info=new android.hardware.display.BrightnessInfo();info.adjustedBrightness=brightness;return info; }}''',
"android/hardware/display/DisplayManager.java": '''package android.hardware.display;
 public class DisplayManager { public final android.view.Display display=new android.view.Display();
 public static final long EVENT_TYPE_DISPLAY_ADDED=1,EVENT_TYPE_DISPLAY_CHANGED=4,
 EVENT_TYPE_DISPLAY_REMOVED=2,EVENT_TYPE_DISPLAY_STATE=16,EVENT_TYPE_DISPLAY_REFRESH_RATE=8,EVENT_TYPE_DISPLAY_BRIGHTNESS=32,
 PRIVATE_EVENT_TYPE_DISPLAY_COMMITTED_STATE_CHANGED=4;
 public long publicEvents,privateEvents;public int committedOnlyCallbacks;
 private DisplayListener listener; private android.os.Handler handler;
 public interface DisplayListener { void onDisplayAdded(int id);void onDisplayRemoved(int id);void onDisplayChanged(int id); }
 public android.view.Display getDisplay(int id) { return display; } public float fallbackBrightness=.4f; public float getBrightness(int id) { return fallbackBrightness; }
 public void registerDisplayListener(DisplayListener l,android.os.Handler h) {
 registerDisplayListener(l,h,EVENT_TYPE_DISPLAY_ADDED|EVENT_TYPE_DISPLAY_CHANGED|EVENT_TYPE_DISPLAY_REMOVED,0); }
 public void registerDisplayListener(DisplayListener l,android.os.Handler h,long events,long privateFlags) {
 listener=l;handler=h;publicEvents=events;privateEvents=privateFlags; }
 public void brightness(float value) { display.brightness=value;
 if((publicEvents&EVENT_TYPE_DISPLAY_BRIGHTNESS)!=0)handler.post(()->listener.onDisplayChanged(0)); }
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
 static Runnable afterShow,afterContext,afterPrepare,afterHide;static int timingReads;static String timingMode;
 static String contextMode="none",contextFault="none";static long contextToken,generation;static int contextBegins,contextEnds;
 static boolean failReset,failUiOn;static final java.util.List<String> events=new java.util.ArrayList<>();
 static final java.util.List<String> shownModes=new java.util.ArrayList<>();
 record BufferKey(int width,int height,float x,float y,float rx,float ry,float alpha) {}
 static final java.util.List<BufferKey> cache=new java.util.ArrayList<>();
 static final java.util.List<BufferKey> prepareKeys=new java.util.ArrayList<>();
 static final java.util.List<Long> prepareStarts=new java.util.ArrayList<>();
 static long prepareEpoch;static int prepareCalls,prepareHits;static String prepareFault="none";static BufferKey readyKey;
 static long cancelPreparation(){return ++prepareEpoch;}
 static boolean hasBuffer(int w,int h,float x,float y,float rx,float ry,float a){
  BufferKey key=new BufferKey(w,h,x,y,rx,ry,a);return key.equals(readyKey)||cache.contains(key);}
 static boolean prepare(int w,int h,float x,float y,float rx,float ry,float a,long token){
  if(!HandlerBackground())throw new AssertionError("preparation must run on its dedicated background looper");
  if(token!=prepareEpoch)return false;
  prepareCalls++;prepareKeys.add(new BufferKey(w,h,x,y,rx,ry,a));prepareStarts.add(android.os.SystemClock.now);
  if(prepareFault.equals("throw"))throw new IllegalStateException("injected preparation error");
  afterPrepare.run();if(token!=prepareEpoch||prepareFault.equals("fail"))return false;
  readyKey=new BufferKey(w,h,x,y,rx,ry,a);return true;}
 static boolean HandlerBackground(){return android.os.Handler.inBackground;}
 static void onShow(int w,int h,float x,float y,float rx,float ry,float a){
  if(HandlerBackground())throw new AssertionError("show must not run on the preparer");
  BufferKey key=new BufferKey(w,h,x,y,rx,ry,a);if(key.equals(readyKey)){prepareHits++;readyKey=null;}
  cache.remove(key);cache.add(0,key);while(cache.size()>2)cache.remove(cache.size()-1);}
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
  afterShow=afterContext=afterPrepare=afterHide=()->{};prepareCalls=prepareHits=0;prepareEpoch=0;prepareFault="none";readyKey=null;cache.clear();prepareKeys.clear();prepareStarts.clear();timingMode="missing";contextMode="none";contextFault="none";
  contextToken=0;contextBegins=contextEnds=0;failReset=failUiOn=false;events.clear();shownModes.clear(); }
 static String readHbmTiming() throws java.io.IOException { timingReads++;
  if(timingMode.equals("missing"))throw new java.nio.file.NoSuchFileException("hbm_timing");
  if(timingMode.equals("denied"))throw new java.nio.file.AccessDeniedException("hbm_timing");
  if(timingMode.equals("security"))throw new SecurityException("hbm_timing");
  return timingMode; }
 static void write(String path,boolean on) throws java.io.IOException {
  if(path.endsWith("/hbm")){events.add(on?"hbm1":"hbm0");if(on)throw new AssertionError("context must never force HBM on");
   if(failReset)throw new java.io.IOException("reset failed");hbm=false;contextToken=0;contextMode="none";
  }else{events.add(on?"ui1":"ui0");if(on&&failUiOn)throw new java.io.IOException("UI-ready failed");ui=on;} }
 static String readHbm() { return hbm?"1":"0"; }}''',
}

harness = '''package org.lineageos.tetris.udfps;
import android.app.Application;import android.os.*;import android.view.Display;
import android.hardware.display.DisplayManager;import vendor.nothing.hardware.udfps.*;
public final class DisplayLifecycleTest {
 static PowerManager power;static DisplayManager displays;static IIllumination service;static IlluminationApplication app;
 static class Client extends Binder implements IIlluminationCallback {
  int failures,links;IBinder.DeathRecipient death;public IBinder asBinder(){return this;}public void onFailure(){failures++;}
  @Override public void linkToDeath(IBinder.DeathRecipient recipient,int flags){links++;death=recipient;}
  @Override public boolean unlinkToDeath(IBinder.DeathRecipient recipient,int flags){if(death==recipient)death=null;return true;}
  void die(){IBinder.DeathRecipient d=death;if(d!=null)d.binderDied();}}
 static int assertions;static void check(boolean ok,String message){assertions++;if(!ok)throw new AssertionError(message);}
 static Client setup(int state){return setup(state,true);}
 static Client setup(int state,boolean powerAvailable){
  Handler.reset();TestHooks.reset();ServiceManager.service=null;service=null;power=new PowerManager();
  power.interactive=state==Display.STATE_ON;
  displays=new DisplayManager();displays.display.state=state;displays.display.committedState=state;
  Application.services.clear();Application.broadcasts.clear();Application.mainQueue.clear();Application.broadcastFailure=null;Application.executorFailure=null;
  Application.afterBroadcast=()->{};Application.beforeBroadcast=intent->{
   check(!Thread.holdsLock(ownerLock()),"broadcast IPC must not hold the capture ownership lock");
   check(TestHooks.ui&&TestHooks.hbm,"visible pulse must be sent only after HBM and UI-ready succeed");
   check(dump().contains("state=illuminating"),"capture state must be illuminating before optional broadcast");
   TestHooks.events.add("aod-pulse");};
  if(powerAvailable)Application.services.put(PowerManager.class,power);
  Application.services.put(DisplayManager.class,displays);
  app=new IlluminationApplication();app.onCreate();Handler.runDue();service=(IIllumination)ServiceManager.service;
  check(service!=null,"startup must register a fresh illumination service");
  return new Client();}
 static void begin(Client client){service.begin(client,540,2109,93);}
 static Object ownerLock(){try{var field=IlluminationApplication.class.getDeclaredField("mLock");
  field.setAccessible(true);return field.get(app);}catch(ReflectiveOperationException e){throw new AssertionError(e);}}
 static Object ownerRequest(){try{var field=IlluminationApplication.class.getDeclaredField("mOwner");
  field.setAccessible(true);return field.get(app);}catch(ReflectiveOperationException e){throw new AssertionError(e);}}
 static void readyAgain(Object request){try{var method=IlluminationApplication.class.getDeclaredMethod("ready",request.getClass());
  method.setAccessible(true);method.invoke(app,request);}catch(ReflectiveOperationException e){throw new AssertionError(e);}}
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
  check(dump().contains("state=waiting for display")&&dump().contains("display=state3"),"DOZE wait must be visible in diagnostics");
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
    &&dump().contains("state=waiting for display"),
    "diagnostics must distinguish requested ON from uncommitted display power");
  displays.request(Display.STATE_ON);Handler.runDue();Handler.advance(123);
  check(TestHooks.shows==0&&c.failures==0&&ownerGeneration().equals(generation),
    "another requested ON event cannot bypass the committed-state gate");
  displays.commit(Display.STATE_ON);Handler.runDue();
  check(displays.committedOnlyCallbacks==1&&TestHooks.shows==1&&TestHooks.hbm&&!TestHooks.ui
    &&c.failures==0&&c.links==1&&ownerGeneration().equals(generation),
    "committed-only event must render the same contact without a basic display-changed event");
  check(dump().contains("displayCommitted=state2")&&dump().contains("display=state2")
    &&dump().contains("state=waiting for panel"),
    "diagnostics must report committed ON while panel settling still gates capture");
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
  check(c.failures==0&&TestHooks.contextBegins==1&&power.interactiveQueries==2
    &&TestHooks.shownModes.equals(java.util.List.of("interactive")),
    "already interactive ON capture must publish exactly one interactive token before rendering");
  long first=TestHooks.contextToken;check(first>0,"scan token must be positive");
  begin(c);Handler.runDue();check(TestHooks.contextBegins==1,"duplicate owner begin must not renew context");
  service.end(c);Handler.runDue();
  int hide=TestHooks.events.lastIndexOf("hide"),off=TestHooks.events.lastIndexOf("hbm0");
  int end=TestHooks.events.lastIndexOf("scan_v1 end "+first);
  check(hide<off&&off<end&&TestHooks.contextToken==0&&!TestHooks.hbm,
    "end token must follow presented hide and HBM0 cleanup");
  c=setup(Display.STATE_ON);power.interactive=false;begin(c);Handler.runDue();
  check(TestHooks.contextBegins==1&&power.interactiveQueries==2
    &&TestHooks.shownModes.equals(java.util.List.of("ambient")),
    "a display already ON during a known noninteractive AOD pulse must remain ambient");
  power.interactive=true;service.end(c);Handler.runDue();
  check(TestHooks.contextBegins==1&&TestHooks.contextToken==0,"unlock before OFF must not reclassify the token");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();power.interactive=true;
  displays.change(Display.STATE_ON);Handler.runDue();
  check(TestHooks.contextBegins==1&&power.interactiveQueries==2
    &&TestHooks.shownModes.equals(java.util.List.of("ambient")),
    "known noninteractive origin must never upgrade after display wake");
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
  System.out.println("PASS: scan context interactive/AOD origin, no upgrade, downgrade, cancellation, replacement, death, old-kernel fallback and ambiguous-write reset");
 }
 static void scanContextUnknownPower(){
  for(boolean initiallyInteractive:new boolean[]{false,true})for(int failedQuery:new int[]{1,2}){
   Client c=setup(Display.STATE_ON);power.interactive=initiallyInteractive;power.failQueryAt=failedQuery;
   begin(c);Handler.runDue();
   check(power.interactiveQueries==2,"both initial and final power state must be sampled, including ambient origin");
   check(c.failures==0&&TestHooks.contextBegins==0&&TestHooks.contextToken==0
     &&TestHooks.shownModes.equals(java.util.List.of("none"))&&TestHooks.hbm&&!TestHooks.ui&&power.lock.isHeld(),
     "unknown initial/final power state must skip all context publication and preserve baseline presentation");
   check(dump().contains("skipped=power_state_unavailable"),"unknown power classification must be explicit in diagnostics");
   Handler.advance(16);check(!TestHooks.ui,"unknown classification must retain the panel settle interval");
   Handler.advance(1);Application.runMain();
   check(c.failures==0&&TestHooks.ui&&TestHooks.hbm&&power.lock.isHeld(),
     "unknown classification must not fail optical readiness or capture");
   check(Application.broadcasts.size()==(!initiallyInteractive&&failedQuery==2?1:0),
     "unknown context classification must preserve existing initial-state AOD pulse policy");
   service.end(c);Handler.runDue();
   check(TestHooks.contextToken==0&&!TestHooks.ui&&!TestHooks.hbm&&!power.lock.isHeld(),
     "capture without context must retain normal cleanup");
  }
  for(int failedQuery:new int[]{1,2}){
   Client previous=setup(Display.STATE_ON);begin(previous);Handler.runDue();Handler.advance(17);
   long previousToken=TestHooks.contextToken;
   power.failQueryAt=power.interactiveQueries+failedQuery;Client unknown=new Client();begin(unknown);Handler.runDue();Handler.advance(17);
   check(unknown.failures==0&&TestHooks.contextBegins==1&&TestHooks.contextToken==0&&TestHooks.ui
     &&TestHooks.shownModes.equals(java.util.List.of("interactive","none"))
     &&TestHooks.events.contains("scan_v1 end "+previousToken),
     "unknown replacement must revoke the previous token and capture without inheriting its mode");
   Client known=new Client();begin(known);Handler.runDue();Handler.advance(17);long knownToken=TestHooks.contextToken;
   check(knownToken>previousToken&&TestHooks.contextBegins==2&&TestHooks.contextMode.equals("interactive"),
     "a later known owner must publish its own context after an unknown replacement");
   previous.die();unknown.die();service.end(previous);service.end(unknown);Handler.runDue();
   check(TestHooks.contextToken==knownToken&&TestHooks.hbm&&TestHooks.ui,
     "stale owners must not revoke the newer known owner's token");
   service.end(known);Handler.runDue();
  }
  System.out.println("PASS: unknown initial/final power state skips all scan context, preserves capture/settle/AOD policy, revokes prior token and protects newer owner");
 }
 static float expectedAlpha(float brightness){
  android.content.res.Resources r=new android.content.res.Resources();
  return new Calibration(r.getIntArray(0),256,2680,4095,4).alpha(brightness);}
 static Client idleWithGeometry(){
  Client c=setup(Display.STATE_ON);begin(c);Handler.runDue();Handler.advance(17);
  service.end(c);Handler.runDue();
  check(!TestHooks.hbm&&!TestHooks.ui&&!power.lock.isHeld(),"preparation fixture requires completed cleanup");
  return c;}
 static void brightness(float value){displays.brightness(value);Handler.runDue();}
 static void queuePreparation(float value){brightness(value);Handler.advance(200);
  check(Handler.backgroundPending()==1,"one stable key must enqueue one background preparation");}
 static void completePreparation(){check(Handler.runOneBackground(),"expected background preparation");Handler.runDue();}
 static Object preparationKey(){try{
  var method=IlluminationApplication.class.getDeclaredMethod("readPreparationKey");method.setAccessible(true);
  return method.invoke(app);
 }catch(ReflectiveOperationException e){throw new AssertionError(e);}}
 static void preparationEligibility(){
  Client c=setup(Display.STATE_ON);
  check((displays.publicEvents&DisplayManager.EVENT_TYPE_DISPLAY_BRIGHTNESS)!=0,
    "preparation must subscribe to brightness-only events");
  brightness(.2f);Handler.advance(5000);
  check(Handler.backgroundPending()==0&&TestHooks.prepareCalls==0&&TestHooks.shows==0,
    "no speculative work before validated HAL geometry and power dependency are known");
  try{service.begin(c,-1,2109,93);throw new AssertionError("invalid geometry accepted");}
  catch(IllegalArgumentException expected){}
  brightness(.3f);Handler.advance(2000);check(Handler.backgroundPending()==0,"invalid HAL geometry must not enable preparation");
  idleWithGeometry();brightness(.2f);displays.request(Display.STATE_OFF);Handler.runDue();Handler.advance(2000);
  check(Handler.backgroundPending()==0&&TestHooks.prepareCalls==0,"requested OFF cancels stable-key preparation");
  idleWithGeometry();brightness(.2f);displays.commit(Display.STATE_OFF);Handler.runDue();Handler.advance(2000);
  check(Handler.backgroundPending()==0,"uncommitted display cannot prepare");
  idleWithGeometry();power.interactive=false;brightness(.2f);Handler.advance(2000);
  check(Handler.backgroundPending()==0,"noninteractive pulse ON cannot speculate");
  idleWithGeometry();displays.display.brightness=Float.NaN;displays.fallbackBrightness=Float.NaN;
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(2000);
  check(Handler.backgroundPending()==0,"unknown adjusted and fallback brightness must not prepare an approximate alpha");
  idleWithGeometry();power.failQueryAt=power.interactiveQueries+1;brightness(.2f);Handler.advance(2000);
  check(Handler.backgroundPending()==0,"power query failure must skip optional preparation");
  c=setup(Display.STATE_ON);begin(c);Handler.runDue();Handler.advance(17);brightness(.2f);Handler.advance(2000);
  check(Handler.backgroundPending()==0&&TestHooks.prepareCalls==0&&TestHooks.ui,"active capture must never start preparation");
  TestHooks.afterHide=()->{
   check(preparationKey()==null,"owner-null cleanup must remain ineligible until hardware reset has finished");
   displays.brightness(.3f);
   check(Handler.backgroundPending()==0,"cleanup may not dispatch producer before finishing hide");};
  service.end(c);Handler.runDue();TestHooks.afterHide=()->{};Handler.advance(200);
  check(Handler.backgroundPending()==1,"preparation resumes only after successful completed cleanup");
  c=setup(Display.STATE_ON);begin(c);Handler.runDue();Handler.advance(17);
  TestHooks.hideSucceeded=false;service.end(c);Handler.runDue();brightness(.2f);Handler.advance(2000);
  check(Handler.backgroundPending()==0&&dump().contains("hardware reset failed"),
    "failed cleanup must not become eligible merely because owner is null");
  System.out.println("PASS: prewarm geometry/power/brightness/interactive/committed gates, active scan and cleanup exclusion");
 }
 static void preparationCoalescingAndReuse(){
  idleWithGeometry();
  for(int i=0;i<50;i++){brightness(.1f+i*.01f);Handler.advance(10);
   check(Handler.backgroundPending()==0,"brightness animation must keep yielding until the key is stable");
   check(Handler.foregroundPending()<=4,"brightness flood must replace its debounce callback instead of accumulating jobs");}
  brightness(.6f);Handler.advance(199);check(Handler.backgroundPending()==0,"stable-key debounce is at least200ms");
  Handler.advance(1);check(Handler.backgroundPending()==1&&TestHooks.prepareCalls==0,
    "producer work must be isolated from the foreground worker queue");
  completePreparation();
  check(TestHooks.prepareCalls==1&&TestHooks.prepareKeys.get(0).alpha()==expectedAlpha(.6f),
    "brightness flood must raster only its final exact calibrated alpha");
  for(int i=0;i<30;i++)brightness(.600001f);
  Handler.advance(2000);check(Handler.backgroundPending()==0&&TestHooks.prepareCalls==1,
    "brightness changes mapping to the same alpha must reuse the completed immutable key");
  Client next=new Client();int shows=TestHooks.shows;begin(next);Handler.runDue();Handler.advance(17);
  check(TestHooks.shows==shows+1&&TestHooks.prepareHits==1&&TestHooks.ui&&next.failures==0,
    "begin cancellation must preserve completed exact-key prewarm for normal show consumption");
  service.end(next);Handler.runDue();
  System.out.println("PASS: brightness flood coalescing,200ms stable key, background isolation, exact alpha and warm reuse across begin");
 }
 static void preparationTouchAndStaleCompletion(){
  idleWithGeometry();queuePreparation(.2f);Client c=new Client();
  begin(c);Handler.runDue();Handler.advance(17);
  check(TestHooks.shows==2&&TestHooks.ui&&TestHooks.prepareCalls==0&&Handler.backgroundPending()==1,
    "touch must show immediately without waiting for an undispatched producer");
  completePreparation();check(TestHooks.prepareCalls==0&&TestHooks.ui&&c.failures==0,
    "cancelled queued producer must not render or modify an active owner");
  service.end(c);Handler.runDue();
  idleWithGeometry();queuePreparation(.2f);Client during=new Client();
  TestHooks.afterPrepare=()->{begin(during);Handler.runDue();Handler.advance(17);
   check(TestHooks.shows==2&&TestHooks.ui,"touch during producer must use normal show without joining preparation");};
  completePreparation();TestHooks.afterPrepare=()->{};
  check(TestHooks.readyKey==null&&during.failures==0&&TestHooks.ui&&dump().contains("owner=true"),
    "stale producer completion must not publish or clear active scan ownership");
  service.end(during);Handler.runDue();
  idleWithGeometry();queuePreparation(.2f);
  TestHooks.afterPrepare=()->{brightness(.3f);Handler.advance(200);
   check(Handler.backgroundPending()==0,"one running producer prevents scheduling a second while key changes");};
  completePreparation();TestHooks.afterPrepare=()->{};
  check(TestHooks.readyKey==null,"changed-key result must be discarded");
  Handler.advance(799);check(Handler.backgroundPending()==0,"new key must respect1000ms start interval");
  Handler.advance(1);check(Handler.backgroundPending()==1,"only latest key should follow cancelled completion");
  completePreparation();
  check(TestHooks.prepareCalls==2&&TestHooks.prepareKeys.get(1).alpha()==expectedAlpha(.3f)
    &&TestHooks.prepareStarts.get(1)-TestHooks.prepareStarts.get(0)>=1000,
    "coalesced retry must retain final alpha and minimum producer interval");
  idleWithGeometry();queuePreparation(.2f);
  TestHooks.afterPrepare=()->{displays.change(Display.STATE_OFF);Handler.runDue();};
  completePreparation();TestHooks.afterPrepare=()->{};Handler.advance(2000);
  check(TestHooks.readyKey==null&&Handler.backgroundPending()==0,"screen off must discard result and suppress rescheduling");
  System.out.println("PASS: touch never waits for queued/running preparation, stale owner/key/off cancellation and latest-only rescheduling");
 }
 static void preparationDelayedWorkerRateLimit(){
  idleWithGeometry();queuePreparation(.2f);
  Handler.advance(1500);
  TestHooks.afterPrepare=()->brightness(.3f);
  completePreparation();TestHooks.afterPrepare=()->{};
  check(TestHooks.prepareCalls==1&&TestHooks.readyKey==null,
    "delayed producer must discard a key superseded while it runs");
  Handler.advance(999);check(Handler.backgroundPending()==0,
    "1000ms rate limit must start at actual background execution, not earlier dispatch");
  Handler.advance(1);check(Handler.backgroundPending()==1,"latest key should resume after actual-start rate limit");
  completePreparation();
  check(TestHooks.prepareStarts.get(1)-TestHooks.prepareStarts.get(0)>=1000,
    "two raster starts must remain at least1000ms apart even after background scheduling delay");
  idleWithGeometry();queuePreparation(.2f);
  for(int i=0;i<20;i++){brightness(.3f+i*.01f);Handler.advance(10);
   check(Handler.backgroundPending()==1,"in-flight cancellation must retain one completion but never enqueue replacements");}
  completePreparation();check(TestHooks.prepareCalls==0,"obsolete undispatched job must exit before raster");
  Handler.advance(999);check(Handler.backgroundPending()==0,"cancelled-job completion retains interval budget");
  Handler.advance(1);check(Handler.backgroundPending()==1,"only final pending key resumes after cancelled job");
  completePreparation();check(TestHooks.prepareCalls==1&&TestHooks.prepareKeys.get(0).alpha()==expectedAlpha(.49f),
    "many changes during a pending job must coalesce to exactly the latest key");
  System.out.println("PASS: actual background-start rate limit after delayed dispatch and one pending latest key under cancellation flood");
 }
 static void preparationFailuresAndGeometry(){
  for(String fault:new String[]{"fail","throw"}){
   idleWithGeometry();TestHooks.prepareFault=fault;queuePreparation(.2f);completePreparation();
   check(TestHooks.prepareCalls==1&&Handler.backgroundPending()==0,"optional native failure must return control");
   for(int i=0;i<6;i++){brightness(.2f);Handler.advance(1000);}
   check(TestHooks.prepareCalls==1&&Handler.backgroundPending()==0,
    "same failed key must not cause a retry loop after "+fault);
   TestHooks.prepareFault="none";queuePreparation(.3f);completePreparation();
   check(TestHooks.prepareCalls==2&&TestHooks.readyKey!=null,"new key must recover after optional preparation failure");
   Client c=new Client();begin(c);Handler.runDue();Handler.advance(17);
   check(c.failures==0&&TestHooks.ui,"background failure must not poison normal scanning");service.end(c);Handler.runDue();
  }
  idleWithGeometry();displays.display.missingBrightnessInfo=true;displays.fallbackBrightness=.2f;
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(200);completePreparation();
  check(TestHooks.prepareKeys.get(0).alpha()==expectedAlpha(.2f),"prewarm must share the normal brightness fallback");
  idleWithGeometry();displays.display.width=2400;displays.display.height=1080;displays.display.rotation=1;
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(200);completePreparation();
  TestHooks.BufferKey key=TestHooks.prepareKeys.get(0);
  SensorGeometry geometry=new SensorGeometry(540,2109,93,2400,1080,1);
  check(key.width()==2400&&key.height()==1080&&key.x()==geometry.x&&key.y()==geometry.y
    &&key.rx()==geometry.radiusX&&key.ry()==geometry.radiusY,
    "prewarm must transform learned physical HAL geometry exactly for display rotation");
  brightness(.3f);Handler.advance(1000);completePreparation();
  Client c=new Client();displays.display.rotation=3;begin(c);Handler.runDue();Handler.advance(17);
  check(TestHooks.prepareHits==0&&c.failures==0&&TestHooks.ui,
    "rotation mismatch must use normal rendering rather than a stale prepared shape");
  System.out.println("PASS: optional producer errors do not crash or retry-spin, new-key recovery, fallback brightness and exact transformed geometry");
 }
 static Client ambientReady(){
  Client c=setup(Display.STATE_OFF);begin(c);Handler.runDue();
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
  check(c.failures==0&&TestHooks.hbm&&TestHooks.ui,"ambient fixture must retain successful first-contact readiness");
  return c;}
 static void noPulse(String why){Application.runMain();check(Application.broadcasts.isEmpty(),why);}
 static void verifyPulseTarget(){
  check(Application.broadcasts.size()==1,"one active ambient request must emit exactly one broadcast");
  Application.Broadcast sent=Application.broadcasts.get(0);
  check("com.android.systemui.doze.pulse".equals(sent.intent().getAction()),"pulse must use the existing SystemUI action");
  check("com.android.systemui".equals(sent.intent().getPackage()),"pulse must target only SystemUI");
  check(sent.user()==UserHandle.SYSTEM&&sent.user().getIdentifier()==0,"pulse must target the system-user receiver");
  check(sent.intent().getFlags()==(android.content.Intent.FLAG_RECEIVER_REGISTERED_ONLY
    |android.content.Intent.FLAG_RECEIVER_FOREGROUND),"pulse must reach an already registered receiver on the foreground broadcast queue");
  check(power.wakes==0,"optional pulse must not replace Doze with full wake");}
 static void visiblePulseReadinessAndOnce(){
  Client c=setup(Display.STATE_OFF);begin(c);Handler.runDue();
  check(Application.mainQueue.isEmpty()&&Application.broadcasts.isEmpty(),"OFF must not request visible pulse before display and illumination readiness");
  displays.request(Display.STATE_ON);Handler.runDue();Handler.advance(50);
  check(TestHooks.shows==0&&Application.mainQueue.isEmpty(),"requested ON alone cannot queue visible pulse while committed state is OFF");
  displays.commit(Display.STATE_ON);Handler.runDue();
  check(TestHooks.shows==1&&!TestHooks.ui&&Application.mainQueue.isEmpty(),"presentation must not request pulse before sensor readiness");
  Handler.advance(16);check(Application.mainQueue.isEmpty(),"existing panel-settle wait still precedes optional pulse");
  Handler.advance(1);check(TestHooks.ui&&Application.mainQueue.size()==1&&Application.broadcasts.isEmpty(),
    "UI-ready must complete before a nonblocking main-executor dispatch is queued");
  Object request=ownerRequest();readyAgain(request);displays.change(Display.STATE_ON);Handler.runDue();
  check(Application.mainQueue.size()==1,"repeated readiness and display events must not duplicate the queued request");
  power.beforeInteractive=()->check(!Thread.holdsLock(ownerLock()),"dispatch power IPC must occur outside ownership lock");
  Application.runMain();power.beforeInteractive=()->{};verifyPulseTarget();
  check(TestHooks.events.indexOf("aod-pulse")>TestHooks.events.indexOf("ui1"),"pulse send follows successful UI_READY write");
  check(TestHooks.ui&&TestHooks.hbm&&c.failures==0&&dump().contains("requested"),"pulse must preserve capture and report a request, not claim visible acknowledgement");
  readyAgain(request);Application.runMain();check(Application.broadcasts.size()==1,"completed dispatch is not renewed in the same request");
  service.end(c);Handler.runDue();
  Client next=new Client();begin(next);Handler.runDue();Handler.advance(17);Application.runMain();
  check(Application.broadcasts.size()==2&&next.failures==0,"a later real contact may request its own one-shot pulse");
  System.out.println("PASS: visible pulse queued after committed ON/HBM/UI-ready, no scan-worker IPC, correct action/package/user/flags and one attempt per contact");
 }
 static void visiblePulseOriginGates(){
  Client c=setup(Display.STATE_ON);begin(c);Handler.runDue();Handler.advance(17);
  noPulse("interactive screen-on authentication must not request ambient UI");
  c=setup(Display.STATE_OFF);power.interactive=true;begin(c);Handler.runDue();power.interactive=false;
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
  noPulse("interactive origin must not become eligible when interactivity later changes");
  for(int requested:new int[]{Display.STATE_DOZE,Display.STATE_DOZE_SUSPEND}){
   c=setup(requested);displays.display.committedState=Display.STATE_OFF;begin(c);Handler.runDue();
   displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
   noPulse("existing requested AOD state must be preserved without an extra pulse");
   check(c.failures==0&&TestHooks.ui,"AOD-origin filtering must not stop biometric capture");
  }
  for(int committed:new int[]{Display.STATE_DOZE,Display.STATE_DOZE_SUSPEND}){
   c=setup(Display.STATE_OFF);displays.display.committedState=committed;begin(c);Handler.runDue();
   displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
   noPulse("existing committed AOD state must be preserved without an extra pulse");
   check(c.failures==0&&TestHooks.ui,"committed AOD-origin filtering must not stop capture");
  }
  for(boolean unknownRequested:new boolean[]{false,true}){
   c=setup(Display.STATE_OFF);
   if(unknownRequested)displays.display.state=Display.STATE_UNKNOWN;else displays.display.committedState=Display.STATE_UNKNOWN;
   begin(c);Handler.runDue();displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
   noPulse("unknown initial display state must not authorize optional AOD promotion");
  }
  c=setup(Display.STATE_OFF);power.failQueryAt=1;begin(c);Handler.runDue();
  displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
  noPulse("unknown initial interactivity must retain ambient capture but not authorize optional pulse");
  check(c.failures==0&&TestHooks.ui,"initial power-query failure must preserve established capture fallback");
  c=setup(Display.STATE_ON);power.interactive=false;begin(c);Handler.runDue();Handler.advance(17);Application.runMain();
  verifyPulseTarget();check(c.failures==0&&TestHooks.ui,"an already ON but noninteractive fingerprint pulse may show full UI");
  System.out.println("PASS: interactive and unknown origins excluded, existing requested/committed AOD untouched, noninteractive ON pulse supported");
 }
 static void visiblePulseCancellationAndFailureGates(){
  Client c=setup(Display.STATE_OFF);begin(c);Handler.runDue();service.end(c);displays.change(Display.STATE_ON);Handler.runDue();Handler.advance(17);
  noPulse("contact cancelled before display readiness must not pulse");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();displays.change(Display.STATE_ON);Handler.runDue();service.end(c);Handler.runDue();Handler.advance(17);
  noPulse("contact cancelled before UI-ready must not pulse");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();displays.change(Display.STATE_ON);Handler.runDue();TestHooks.hbm=false;Handler.advance(17);
  noPulse("revoked HBM must fail readiness without sending visible pulse");check(c.failures==1,"revoked HBM remains a capture failure");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();displays.change(Display.STATE_ON);Handler.runDue();TestHooks.failUiOn=true;Handler.advance(17);
  noPulse("failed UI-ready write must not queue visible pulse");check(c.failures==1&&!TestHooks.ui,"UI-ready write failure must remain visible");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();displays.change(Display.STATE_ON);Handler.runDue();displays.display.rotation=1;Handler.advance(17);
  noPulse("changed geometry before readiness must not pulse");
  c=setup(Display.STATE_OFF);begin(c);Handler.runDue();Handler.advance(2000);
  noPulse("display timeout must not leave a queued visible pulse");
  c=ambientReady();check(Application.mainQueue.size()==1,"fixture needs queued pulse");service.end(c);Handler.runDue();
  noPulse("release while main dispatch is pending must cancel visible pulse");
  c=ambientReady();c.die();Handler.runDue();noPulse("owner death must cancel queued pulse");
  c=ambientReady();power.interactive=true;noPulse("unlock before main dispatch must suppress late ambient pulse");
  check(c.failures==0&&TestHooks.ui,"suppressed UI request must not disturb ongoing capture");
  c=ambientReady();displays.display.state=Display.STATE_OFF;noPulse("requested power loss before dispatch suppresses pulse");
  c=ambientReady();displays.display.committedState=Display.STATE_OFF;noPulse("committed power loss before dispatch suppresses pulse");
  Client old=ambientReady();service.end(old);Client newer=new Client();begin(newer);Handler.runDue();Handler.advance(17);
  check(Application.mainQueue.size()==2,"replacement fixture retains both callbacks for ownership testing");
  check(Application.runOneMain()&&Application.broadcasts.isEmpty(),"stale main callback cannot send for a replacement owner");
  Application.runMain();verifyPulseTarget();check(newer.failures==0&&TestHooks.ui,"new owner remains able to send its own pulse");
  c=ambientReady();final Client cancelledDuringSample=c;
  power.beforeInteractive=()->{power.beforeInteractive=()->{};service.end(cancelledDuringSample);Handler.runDue();};
  noPulse("ownership must be rechecked after power/display IPC before dispatch decision");
  check(dump().contains("owner=false")&&!TestHooks.hbm&&!TestHooks.ui,"sampling race must allow normal cancellation");
  c=ambientReady();final Client replacedDuringSample=c;final Client activeAfterSample=new Client();
  power.beforeInteractive=()->{power.beforeInteractive=()->{};service.end(replacedDuringSample);
   begin(activeAfterSample);Handler.runDue();Handler.advance(17);};
  check(Application.runOneMain()&&Application.broadcasts.isEmpty(),
    "old dispatch must not borrow a replacement owner's illuminated state after sampling IPC");
  Application.runMain();verifyPulseTarget();
  check(activeAfterSample.failures==0&&TestHooks.ui,"replacement during sampling keeps its own one-shot pulse");
  System.out.println("PASS: no pulse after failed readiness, cancellation, death, unlock, power loss, timeout or stale owner; main sampling races recheck ownership");
 }
 static void visiblePulseOptionalFailures(){
  Client c=setup(Display.STATE_OFF);begin(c);Handler.runDue();displays.change(Display.STATE_ON);Handler.runDue();
  Application.executorFailure=new java.util.concurrent.RejectedExecutionException("executor unavailable");Handler.advance(17);
  check(c.failures==0&&TestHooks.ui&&TestHooks.hbm&&Application.mainQueue.isEmpty()&&dump().contains("failed"),
    "executor rejection must be diagnostic only, after successful readiness");
  Application.executorFailure=null;readyAgain(ownerRequest());Application.runMain();
  check(Application.broadcasts.isEmpty(),"failed enqueue may not retry repeatedly in the same contact");
  for(RuntimeException fault:new RuntimeException[]{new SecurityException("broadcast denied"),new IllegalStateException("receiver unavailable")}){
   c=ambientReady();Application.broadcastFailure=fault;Application.runMain();
   check(Application.broadcasts.size()==1&&c.failures==0&&TestHooks.ui&&TestHooks.hbm&&dump().contains("failed"),
    "broadcast failure must not fail or cancel fingerprint capture");
   Application.broadcastFailure=null;readyAgain(ownerRequest());Application.runMain();
   check(Application.broadcasts.size()==1,"failed send must remain one attempt for the contact");
  }
  c=ambientReady();power.failQueryAt=power.interactiveQueries+1;noPulse("unavailable current interactivity must fail closed for optional pulse");
  check(c.failures==0&&TestHooks.ui,"dispatch dependency error must not fail biometric capture");
  c=ambientReady();final Client releasedInIpc=c;
  Application.afterBroadcast=()->{service.end(releasedInIpc);Handler.runDue();};
  Application.runMain();
  check(Application.broadcasts.size()==1&&c.failures==0&&!TestHooks.ui&&!TestHooks.hbm&&dump().contains("owner=false"),
    "an already-decided broadcast may finish after release but cannot block or revive capture");
  System.out.println("PASS: executor/broadcast/dependency failures remain optional, never retry-spin, and broadcast IPC permits cancellation");
 }
 public static void main(String[] args){
  visiblePulseReadinessAndOnce();visiblePulseOriginGates();
  visiblePulseCancellationAndFailureGates();visiblePulseOptionalFailures();
  preparationEligibility();preparationCoalescingAndReuse();
  preparationTouchAndStaleCompletion();preparationDelayedWorkerRateLimit();preparationFailuresAndGeometry();
  scanContextLifecycle();
  scanContextUnknownPower();
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
  check(dump().contains("state=waiting for display")&&dump().contains("display=state1")
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
  System.out.println("PASS: "+assertions+" assertions against the real Java application");
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
