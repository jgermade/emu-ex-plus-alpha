/*  This file is part of Imagine.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Imagine.  If not, see <http://www.gnu.org/licenses/> */

#define LOGTAG "Base"
#include <cstdlib>
#include <imagine/logger/logger.h>
#include <imagine/engine-globals.h>
#include <imagine/base/android/sdk.hh>
#include <imagine/base/Base.hh>
#include <imagine/base/Timer.hh>
#include "../common/windowPrivate.hh"
#include "../common/basePrivate.hh"
#include <imagine/gfx/Gfx.hh>
#include "android.hh"
#include <android/window.h>
#include <android/configuration.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <dlfcn.h>
#include <imagine/fs/sys.hh>
#include <imagine/util/fd-utils.h>
#include <imagine/util/bits.h>
#ifdef CONFIG_BLUETOOTH
#include <imagine/bluetooth/BluetoothInputDevScanner.hh>
#endif
#include "../../input/android/private.hh"
#include "internal.hh"

namespace Gfx
{

AndroidSurfaceTextureConfig surfaceTextureConf;

void AndroidSurfaceTextureConfig::init(JNIEnv *jEnv)
{
	if(Base::androidSDK() >= 14)
	{
		//logMsg("setting up SurfaceTexture JNI");
		// Surface members
		jSurfaceCls = (jclass)jEnv->NewGlobalRef(jEnv->FindClass("android/view/Surface"));
		jSurface.setup(jEnv, jSurfaceCls, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
		jSurfaceRelease.setup(jEnv, jSurfaceCls, "release", "()V");
		// SurfaceTexture members
		jSurfaceTextureCls = (jclass)jEnv->NewGlobalRef(jEnv->FindClass("android/graphics/SurfaceTexture"));
		jSurfaceTexture.setup(jEnv, jSurfaceTextureCls, "<init>", "(I)V");
		//jSetDefaultBufferSize.setup(jEnv, jSurfaceTextureCls, "setDefaultBufferSize", "(II)V");
		jUpdateTexImage.setup(jEnv, jSurfaceTextureCls, "updateTexImage", "()V");
		jSurfaceTextureRelease.setup(jEnv, jSurfaceTextureCls, "release", "()V");
		use = 1;
	}
}

void AndroidSurfaceTextureConfig::deinit()
{
	// TODO
	jSurfaceTextureCls = nullptr;
	use = whiteListed = 0;
}

bool supportsAndroidSurfaceTexture() { return surfaceTextureConf.isSupported(); }
bool supportsAndroidSurfaceTextureWhitelisted() { return surfaceTextureConf.isSupported() && surfaceTextureConf.whiteListed; };
bool useAndroidSurfaceTexture() { return surfaceTextureConf.isSupported() ? surfaceTextureConf.use : 0; };
void setUseAndroidSurfaceTexture(bool on)
{
	if(surfaceTextureConf.isSupported())
		surfaceTextureConf.use = on;
}

}

namespace Base
{

JavaVM* jVM = nullptr;
static JNIEnv* aJEnv = nullptr;

// activity
jclass jBaseActivityCls = nullptr;
jobject jBaseActivity = nullptr;
uint appState = APP_PAUSED;
bool aHasFocus = true;
static AConfiguration *aConfig = nullptr;
static AAssetManager *assetManager = nullptr;
static JavaInstMethod<void> jSetUIVisibility;
//static JavaInstMethod<void> jFinish;
static JavaInstMethod<jobject> jNewFontRenderer;
JavaInstMethod<void> jSetRequestedOrientation;
const char *appPath = nullptr;
static const char *filesDir = nullptr, *eStoreDir = nullptr;
static uint aSDK = aMinSDK;
static bool osAnimatesRotation = false;
int osOrientation = -1;
static bool hasPermanentMenuKey = true;
static bool keepScreenOn = false;
static Timer userActivityCallback;
static uint uiVisibilityFlags = SYS_UI_STYLE_NO_FLAGS;
uint androidUIInUse = 0;

// window
JavaInstMethod<void> jSetWinFormat, jSetWinFlags;
JavaInstMethod<int> jWinFormat, jWinFlags;

static Base::Screen::OnFrameDelegate restoreGLContextFromAndroidUI
{
	[](Screen &screen, FrameTimeBase)
	{
		// an Android UI element like EditText may make its
		// own context current so make sure setAsDrawTarget restores ours
		restoreOpenGLContext();
		if(androidUIInUse)
		{
			// do callback each frame until UI use stops
			screen.addOnFrameDelegate(restoreGLContextFromAndroidUI);
		}
		else
		{
			logMsg("stopping per-frame context check");
		}
	}
};

void unrefUIGL()
{
	assert(androidUIInUse);
	androidUIInUse--;
	if(!androidUIInUse)
	{
		//logMsg("no more Android UI elements in use");
	}
}

void refUIGL()
{
	androidUIInUse++;
	auto &screen = mainScreen();
	if(!screen.containsOnFrameDelegate(restoreGLContextFromAndroidUI))
	{
		screen.addOnFrameDelegate(restoreGLContextFromAndroidUI);
	}
}

uint appActivityState() { return appState; }

void exit(int returnVal)
{
	// TODO: return exit value as activity result
	logMsg("exiting process");
	appState = APP_EXITING;
	dispatchOnExit(false);
	removePostedNotifications();
	::exit(returnVal);
	//jFinish(env, jBaseActivity);
}
void abort() { ::abort(); }

//void openURL(const char *url) { };

const char *documentsPath() { return filesDir; }
const char *storagePath() { return eStoreDir; }

AAssetManager *activityAAssetManager()
{
	assert(assetManager);
	return assetManager;
}

bool hasHardwareNavButtons()
{
	return hasPermanentMenuKey;
}

uint androidSDK()
{
	return std::max(aMinSDK, aSDK);
}

static bool setOrientationOS(int o)
{
	using namespace Gfx;
	static const Angle orientationDiffTable[4][4] =
	{
			{ 0, angleFromDegree(90), angleFromDegree(-180), angleFromDegree(-90) },
			{ angleFromDegree(-90), 0, angleFromDegree(90), angleFromDegree(-180) },
			{ angleFromDegree(-180), angleFromDegree(-90), 0, angleFromDegree(90) },
			{ angleFromDegree(90), angleFromDegree(-180), angleFromDegree(-90), 0 },
	};

	logMsg("OS orientation change");
	assert(osOrientation != -1);
	if(osOrientation != o)
	{
		if(!osAnimatesRotation)
		{
			auto rotAngle = orientationDiffTable[osOrientation][o];
			logMsg("animating from %d degrees", (int)angleToDegree(rotAngle));
			Gfx::animateProjectionMatrixRotation(rotAngle, 0.);
		}
		//logMsg("new value %d", o);
		osOrientation = o;
		return true;
	}
	return false;
}

bool surfaceTextureSupported()
{
	return Gfx::surfaceTextureConf.isSupported();
}

jobject newFontRenderer(JNIEnv *jEnv)
{
	return jNewFontRenderer(jEnv, jBaseActivity);
}

static void initConfig(AConfiguration* config)
{
	initInputConfig(config);
}

static void configChange(JNIEnv* jEnv, AConfiguration* config)
{
	auto orientation = mainScreen().aOrientation(jEnv);
	if(setOrientationOS(orientation))
	{
		//logMsg("changed OS orientation");
	}
	changeInputConfig(jEnv, config);
}

static void activityInit(JNIEnv* jEnv, jobject activity)
{
	// BaseActivity members
	{
		jBaseActivityCls = (jclass)jEnv->NewGlobalRef(jEnv->GetObjectClass(activity));
		jSetRequestedOrientation.setup(jEnv, jBaseActivityCls, "setRequestedOrientation", "(I)V");
		//jFinish.setup(jEnv, jBaseActivityCls, "finish", "()V");
		#ifdef CONFIG_RESOURCE_FONT_ANDROID
		jNewFontRenderer.setup(jEnv, jBaseActivityCls, "newFontRenderer", "()Lcom/imagine/FontRenderer;");
		#endif

		{
			JNINativeMethod method[] =
			{
				{
					"onContentRectChanged", "(JIIIIII)V",
					(void*)(void JNICALL (*)(JNIEnv* env, jobject thiz, jlong windowAddr, jint x, jint y, jint x2, jint y2, jint winWidth, jint winHeight))
					([](JNIEnv* env, jobject thiz, jlong windowAddr, jint x, jint y, jint x2, jint y2, jint winWidth, jint winHeight)
					{
						auto win = windowAddr ? (Window*)windowAddr : deviceWindow();
						androidWindowContentRectChanged(*win, {x, y, x2, y2}, {winWidth, winHeight});
					})
				}
			};
			jEnv->RegisterNatives(jBaseActivityCls, method, sizeofArray(method));
		}

		if(Base::androidSDK() < 11) // bug in pre-3.0 Android causes paths in ANativeActivity to be null
		{
			logMsg("ignoring paths from ANativeActivity due to Android 2.3 bug");
			JavaInstMethod<jobject> jFilesDir;
			jFilesDir.setup(jEnv, jBaseActivityCls, "filesDir", "()Ljava/lang/String;");
			filesDir = jEnv->GetStringUTFChars((jstring)jFilesDir(jEnv, activity), nullptr);
		}
		{
			JavaClassMethod<jobject> extStorageDir;
			extStorageDir.setup(jEnv, jBaseActivityCls, "extStorageDir", "()Ljava/lang/String;");
			eStoreDir = jEnv->GetStringUTFChars((jstring)extStorageDir(jEnv), nullptr);
		}
		assert(filesDir);
		assert(eStoreDir);
		logMsg("internal storage path: %s", filesDir);
		logMsg("external storage path: %s", eStoreDir);

		doOrAbort(logger_init());
		engineInit();
		logMsg("SDK API Level: %d", aSDK);

		{
			JavaInstMethod<jobject> jApkPath;
			jApkPath.setup(jEnv, jBaseActivityCls, "apkPath", "()Ljava/lang/String;");
			appPath = jEnv->GetStringUTFChars((jstring)jApkPath(jEnv, activity), nullptr);
			logMsg("apk @ %s", appPath);
		}

		if(Base::androidSDK() >= 11)
			osAnimatesRotation = true;
		else
		{
			JavaClassMethod<jboolean> jAnimatesRotation;
			jAnimatesRotation.setup(jEnv, jBaseActivityCls, "gbAnimatesRotation", "()Z");
			osAnimatesRotation = jAnimatesRotation(jEnv);
		}
		if(!osAnimatesRotation)
		{
			logMsg("app handles rotation animations");
		}

		if(!Config::MACHINE_IS_OUYA)
		{
			if(Base::androidSDK() >= 14)
			{
				JavaInstMethod<jboolean> jHasPermanentMenuKey;
				jHasPermanentMenuKey.setup(jEnv, jBaseActivityCls, "hasPermanentMenuKey", "()Z");
				Base::hasPermanentMenuKey = jHasPermanentMenuKey(jEnv, activity);
				if(Base::hasPermanentMenuKey)
				{
					logMsg("device has hardware nav/menu keys");
				}
			}
			else
				Base::hasPermanentMenuKey = 1;
		}
	}

	Gfx::surfaceTextureConf.init(jEnv);

	initScreens(jEnv, activity, jBaseActivityCls);
	initFrameTimer(jEnv, activity);

	jSetWinFlags.setup(jEnv, jBaseActivityCls, "setWinFlags", "(II)V");
	jWinFlags.setup(jEnv, jBaseActivityCls, "winFlags", "()I");
	if(Base::androidSDK() < 11)
	{
		jSetWinFormat.setup(jEnv, jBaseActivityCls, "setWinFormat", "(I)V");
		jWinFormat.setup(jEnv, jBaseActivityCls, "winFormat", "()I");
	}

	if(Base::androidSDK() >= 11)
	{
		jSetUIVisibility.setup(jEnv, jBaseActivityCls, "setUIVisibility", "(I)V");
	}
}

static void dlLoadFuncs()
{
	#if CONFIG_ENV_ANDROID_MINSDK >= 12
	 // no functions from dlopen needed if targeting at least Android 3.1 (SDK 12)
	return;
	#endif
	 // no functions from dlopen needed if running less than Android 3.1 (SDK 12)
	if(Base::androidSDK() < 12)
	{
		return;
	}

	void *libandroid = dlopen("/system/lib/libandroid.so", RTLD_LOCAL | RTLD_LAZY);
	if(!libandroid)
	{
		logWarn("unable to dlopen libandroid.so");
		return;
	}

	Input::dlLoadAndroidFuncs(libandroid);
}

JNIEnv* eEnv() { assert(aJEnv); return aJEnv; }

void setIdleDisplayPowerSave(bool on)
{
	auto env = eEnv();
	jint keepOn = !on;
	auto keepsScreenOn = userActivityCallback ? false : (bool)(jWinFlags(env, jBaseActivity) & AWINDOW_FLAG_KEEP_SCREEN_ON);
	if(keepOn != keepsScreenOn)
	{
		logMsg("keep screen on: %d", keepOn);
		jSetWinFlags(env, jBaseActivity, keepOn ? AWINDOW_FLAG_KEEP_SCREEN_ON : 0, AWINDOW_FLAG_KEEP_SCREEN_ON);
	}
	keepScreenOn = keepOn; // cache value for endIdleByUserActivity()
}

void endIdleByUserActivity()
{
	if(!keepScreenOn)
	{
		//logMsg("signaling user activity to the OS");
		auto env = eEnv();
		// quickly toggle KEEP_SCREEN_ON flag to brighten screen,
		// waiting about 20ms before toggling it back off triggers the screen to brighten if it was already dim
		jSetWinFlags(env, jBaseActivity, AWINDOW_FLAG_KEEP_SCREEN_ON, AWINDOW_FLAG_KEEP_SCREEN_ON);
		userActivityCallback.callbackAfterMSec(
			[env]()
			{
				if(!keepScreenOn)
				{
					jSetWinFlags(env, jBaseActivity, 0, AWINDOW_FLAG_KEEP_SCREEN_ON);
				}
			}, 20);
	}
}

static void setStatusBarHidden(JNIEnv *env, bool hidden)
{
	auto statusBarIsHidden = (bool)(jWinFlags(env, jBaseActivity) & AWINDOW_FLAG_FULLSCREEN);
	if(hidden != statusBarIsHidden)
	{
		logMsg("setting app window fullscreen: %u", hidden);
		jSetWinFlags(env, jBaseActivity, hidden ? AWINDOW_FLAG_FULLSCREEN : 0, AWINDOW_FLAG_FULLSCREEN);
	}
}

void setSysUIStyle(uint flags)
{
	// Flags mapped directly to SYSTEM_UI_FLAG_*
	// SYS_UI_STYLE_DIM_NAV -> SYSTEM_UI_FLAG_LOW_PROFILE (0)
	// SYS_UI_STYLE_HIDE_NAV -> SYSTEM_UI_FLAG_HIDE_NAVIGATION (1)
	// SYS_UI_STYLE_HIDE_STATUS -> SYSTEM_UI_FLAG_FULLSCREEN (2)
	// SYS_UI_STYLE_NO_FLAGS -> SYSTEM_UI_FLAG_VISIBLE (no bits set)

	if(Config::MACHINE_IS_OUYA)
	{
		return;
	}
	auto env = eEnv();
	// Using SYSTEM_UI_FLAG_FULLSCREEN has an odd delay when
	// combined with SYSTEM_UI_FLAG_HIDE_NAVIGATION, so we'll
	// set the window flag even on Android 4.1+.
	// TODO: Re-test on Android versions after 4.4 for any change
	//if(androidSDK() < 16)
	{
		// handle status bar hiding via full-screen window flag
		setStatusBarHidden(env, flags & SYS_UI_STYLE_HIDE_STATUS);
	}
	if(androidSDK() >= 11)
	{
		constexpr uint SYSTEM_UI_FLAG_IMMERSIVE_STICKY = 0x1000;
		// Add SYSTEM_UI_FLAG_IMMERSIVE_STICKY for use with Android 4.4+ if flags contain OS_NAV_STYLE_HIDDEN
		if(flags & SYS_UI_STYLE_HIDE_NAV)
			flags |= SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
		logMsg("setting UI visibility: 0x%X", flags);
		uiVisibilityFlags = flags;
		jSetUIVisibility(env, jBaseActivity, flags);
	}
}

static void setNativeActivityCallbacks(ANativeActivity* activity)
{
	activity->callbacks->onDestroy =
		[](ANativeActivity *)
		{
			::exit(0);
		};
	//activity->callbacks->onStart = nullptr; // unused
	activity->callbacks->onResume =
		[](ANativeActivity *activity)
		{
			logMsg("app resumed");
			appState = APP_RUNNING;
			#ifdef CONFIG_AUDIO
			Audio::updateFocusOnResume();
			#endif
			Window::postNeededScreens();
			#ifdef CONFIG_INPUT_ANDROID_MOGA
			Input::onResumeMOGA(eEnv(), true);
			#endif
			dispatchOnResume(aHasFocus);
			handleIntent(activity);
		};
	//activity->callbacks->onSaveInstanceState = nullptr; // unused
	activity->callbacks->onPause =
		[](ANativeActivity *activity)
		{
			if(appIsRunning())
				appState = APP_PAUSED;
			logMsg("app %s", appState == APP_PAUSED ? "paused" : "exiting");
			Screen::unpostAll();
			dispatchOnExit(appState == APP_PAUSED);
			#ifdef CONFIG_AUDIO
			Audio::updateFocusOnPause();
			#endif
			#ifdef CONFIG_INPUT_ANDROID_MOGA
			Input::onPauseMOGA(activity->env);
			#endif
			Input::deinitKeyRepeatTimer();
		};
	//activity->callbacks->onStop = nullptr; // unused
	activity->callbacks->onConfigurationChanged =
		[](ANativeActivity *activity)
		{
			AConfiguration_fromAssetManager(aConfig, activity->assetManager);
			configChange(activity->env, aConfig);
		};
	activity->callbacks->onLowMemory =
		[](ANativeActivity *)
		{
			dispatchOnFreeCaches();
		};
	activity->callbacks->onWindowFocusChanged =
		[](ANativeActivity *activity, int focused)
		{
			aHasFocus = focused;
			logMsg("focus change: %d", focused);
			if(focused && Base::androidSDK() >= 11)
			{
				// re-apply UI visibility flags
				jSetUIVisibility(activity->env, activity->clazz, uiVisibilityFlags);
			}
			iterateTimes(Window::windows(), i)
			{
				Window::window(i)->dispatchFocusChange(focused);
			}
		};
	activity->callbacks->onNativeWindowCreated =
		[](ANativeActivity *activity, ANativeWindow *nWin)
		{
			if(Base::androidSDK() < 11)
			{
				// In testing with CM7 on a Droid, the surface is re-created in RGBA8888 upon
				// resuming the app no matter what format was used in ANativeWindow_setBuffersGeometry().
				// Explicitly setting the format here seems to fix the problem (Android driver bug?).
				// In case of a mismatch, the surface is usually destroyed & re-created by the OS after this callback.
				#ifndef NDEBUG
				logMsg("setting window format to %d (current %d) after surface creation", deviceWindow()->pixelFormat, ANativeWindow_getFormat(nWin));
				#endif
				jSetWinFormat(activity->env, activity->clazz, deviceWindow()->pixelFormat);
			}
			androidWindowInitSurface(*deviceWindow(), nWin);
		};
	activity->callbacks->onNativeWindowDestroyed =
		[](ANativeActivity *, ANativeWindow *)
		{
			androidWindowSurfaceDestroyed(*deviceWindow());
		};
	//activity->callbacks->onNativeWindowResized = nullptr; // unused
	activity->callbacks->onNativeWindowRedrawNeeded =
		[](ANativeActivity *, ANativeWindow *)
		{
			androidWindowNeedsRedraw(*deviceWindow());
		};
	activity->callbacks->onInputQueueCreated =
		[](ANativeActivity *, AInputQueue *queue)
		{
			inputQueue = queue;
			logMsg("input queue created & attached");
			AInputQueue_attachLooper(queue, activityLooper(), ALOOPER_POLL_CALLBACK,
				[](int, int, void* data)
				{
					processInput((AInputQueue*)data);
					return 1;
				}, queue);
		};
	activity->callbacks->onInputQueueDestroyed =
		[](ANativeActivity *, AInputQueue *queue)
		{
			logMsg("input queue destroyed");
			inputQueue = nullptr;
			AInputQueue_detachLooper(queue);
		};
	//activity->callbacks->onContentRectChanged = nullptr; // unused
}

}

CLINK void LVISIBLE ANativeActivity_onCreate(ANativeActivity* activity, void* savedState, size_t savedStateSize)
{
	using namespace Base;
	logMsg("called ANativeActivity_onCreate, thread ID %d", gettid());
	/*if(aLooper)
	{
		logMsg("skipping onCreate");
		setNativeActivityCallbacks(activity);
		assert(jVM == activity->vm);
		assert(aJEnv == activity->env);
		jBaseActivity = activity->clazz;
		return;
	}*/
	aSDK = activity->sdkVersion;
	if(Base::androidSDK() >= 16)
		processInput = Base::processInputWithGetEvent;
	initActivityLooper();
	jVM = activity->vm;
	assetManager = activity->assetManager;
	jBaseActivity = activity->clazz;
	aJEnv = activity->env;
	filesDir = activity->internalDataPath;
	activityInit(aJEnv, activity->clazz);
	setNativeActivityCallbacks(activity);
	Base::dlLoadFuncs();
	doOrAbort(Input::init());
	aConfig = AConfiguration_new();
	AConfiguration_fromAssetManager(aConfig, activity->assetManager);
	initConfig(aConfig);
	doOrAbort(onInit(0, nullptr));
	if(!Window::windows())
	{
		bug_exit("didn't create a window");
	}
}
