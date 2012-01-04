//headers for OpenNI
#include <XnOpenNI.h>
#include <XnCppWrapper.h>
#include <XnHash.h>
#include <XnLog.h>
#include <ole2.h>
#include <OleAuto.h>
#include <ObjBase.h>
#include <iostream>
#include <string>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <WinError.h>
#include <Guiddef.h>
#include <InitGuid.h>
#include <sstream>
#include <iomanip>
#include <OAIdl.h>
#include <XnVNite.h>
#include "PointDrawer.h"
#include "stereoplayer_h.h"
#import "./Debug/Subversion_Kinect.tlb" named_guids

HRESULT hr;
HRESULT hresult;
int temp;
VARIANT vResult;
VARIANT_BOOL vBoolTrue = true;
VARIANT_BOOL vBoolFalse = false;

bool print_debug = true;

bool play; //indicates if the player is in playbackstate = 0
bool stop; // playback state = 1;
bool pause; //true if the player is currently paused
bool fullScreen; //true if the player is in fullscreen mode
double zoom; //numeric indicator of the current zoom level; default = 100
int circleCount; //number of circle gestures made
bool sliderMode; //true if the left/right slider is being used to track position
int commandState; //the variable that tells the function PrintSessionInstructions what to print on the screen
XnFloat playbackPosition; //a number between 0 and 1 indicating the desired playback start position
double duration; //the duration of the video in seconds
bool ready; //true if the player is in "ready" state

using namespace StereoPlayer;
using namespace std;

//headers for gesture recognition
#include <XnVSwipeDetector.h>
#include <XnVSteadyDetector.h>
#include <XnVCircleDetector.h>
#include <XnVWaveDetector.h>
#include <XnVPushDetector.h>
#include <XnVSelectableSlider1D.h>

#define CHECK_RC(rc, what)											\
	if (rc != XN_STATUS_OK)											\
	{																\
		printf("%s failed: %s\n", what, xnGetStatusString(rc));		\
		return rc;													\
	}																\

#define CHECK_ERRORS(rc, errors, what)		\
	if (rc == XN_STATUS_NO_NODE_PRESENT)	\
	{										\
		XnChar strError[1024];				\
		errors.ToString(strError, 1024);	\
		printf("%s\n", strError);			\
		return (rc);						\
	}

#ifdef USE_GLUT
	#if (XN_PLATFORM == XN_PLATFORM_MACOSX)
		#include <GLUT/glut.h>
	#else
		#include <glut.h>
	#endif
#elif defined(USE_GLES)
	#include "opengles.h"
	#include "kbhit.h"
#endif
#include "signal_catch.h"

#ifdef USE_GLES
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
#endif


//creating the OpenNI objects that get used in the NITE application
xn::Context g_Context;
xn::ScriptNode g_ScriptNode;
xn::DepthGenerator g_DepthGenerator;
xn::HandsGenerator g_HandsGenerator;
xn::GestureGenerator g_GestureGenerator;

//NITE-specific objects
XnVSessionManager* g_pSessionManager;
XnVFlowRouter* g_pFlowRouter;

//the OpenGL drawer
XnVPointDrawer* g_pDrawer;

//instantiating circle detector
XnVCircleDetector* g_pCircle = NULL;

//instantiating swipe detector
XnVSwipeDetector* g_pSwipe = NULL;

//steady detector
XnVSteadyDetector* g_pSteady = NULL;

//wave detector
XnVWaveDetector* g_pWave = NULL;

//push detector
XnVPushDetector* g_pPush = NULL;

//slider detector
XnVSelectableSlider1D* g_pSlider = NULL;

#define GL_WIN_SIZE_X 720
#define GL_WIN_SIZE_Y 480

//Logic for deciding whether or not to render certain pieces
XnBool g_bDrawDepthMap = true;
XnBool g_bPrintFrameID = false;

//use smoothing?
XnFloat g_fSmoothing = 0.8f;
XnBool g_bPause = false;
XnBool g_bQuit = false;

SessionState g_SessionState = NOT_IN_SESSION;

void CleanupExit()
{
	g_ScriptNode.Release();
	g_DepthGenerator.Release();
	g_HandsGenerator.Release();
	g_GestureGenerator.Release();
	g_Context.Release();
	
	StereoPlayer::IAutomationPtr pApp(__uuidof(StereoPlayer::Automation));
	pApp->ClosePlayer();

	cout << "Exiting and closing..." << endl;

	exit(1);
}

//this function gets called when the focus gesture is being performed
void XN_CALLBACK_TYPE FocusProgress(const XnChar* strFocus, const XnPoint3D& ptPosition, XnFloat fProgress, void* UserCxt)
{
	//we won't print or do anything when the user is doing the focus gesturwe
}

//callback function for when the session begin
void XN_CALLBACK_TYPE SessionStarting(const XnPoint3D& ptPosition, void* UserCxt)
{

	if(print_debug)
	{
		printf("Session start: (%f, %f, %f)\n",ptPosition.X, ptPosition.Y, ptPosition.Z);
	}
	g_SessionState = IN_SESSION;
}

//callback for the session getting teminated
void XN_CALLBACK_TYPE SessionEnding(void* UserCxt)
{
	if(print_debug)
	{
		printf("Session end\n");
	}
	g_SessionState = NOT_IN_SESSION;
}

//this function gets called when the system detects that someone has removed their hands from the tracking area
void XN_CALLBACK_TYPE NoHands(void * UserCxt)
{
	if(print_debug)
	{
		printf("Quick refocus state\n");
	}
	commandState = 3;
	g_SessionState = QUICK_REFOCUS;
}

void XN_CALLBACK_TYPE TouchingCallback(xn::HandTouchingFOVEdgeCapability& generator, XnUserID id, const XnPoint3D* pPosition, XnFloat fTime, XnDirection eDir, void* pCookie)
{
	//printf("Hand at edge of FIELD OF VIEW\n");
	commandState = 4;

	g_pDrawer->SetTouchingFOVEdge(id);
}

//the glutDisplay loop gets called on every frame
void glutDisplay (void)
{

	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Setup the OpenGL viewpoint
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();

	XnMapOutputMode mode;
	g_DepthGenerator.GetMapOutputMode(mode);
#ifdef USE_GLUT
	glOrtho(0, mode.nXRes, mode.nYRes, 0, -1.0, 1.0);
#elif defined(USE_GLES)
	glOrthof(0, mode.nXRes, mode.nYRes, 0, -1.0, 1.0);
#endif

	glDisable(GL_TEXTURE_2D);

	if (!g_bPause)
	{
		// Read next available data
		g_Context.WaitOneUpdateAll(g_DepthGenerator);
		// Update NITE tree
		g_pSessionManager->Update(&g_Context);
#ifdef USE_GLUT
		PrintSessionState(g_SessionState);
		PrintSessionInstructions(commandState);
#endif
	}

#ifdef USE_GLUT
	glutSwapBuffers();
#endif
}

#ifdef USE_GLUT
void glutIdle (void)
{
	if (g_bQuit) {
		CleanupExit();
	}

	// Display the frame
	glutPostRedisplay();
}

void glutKeyboard (unsigned char key, int x, int y)
{
	switch (key)
	{
	case 27:
		// Exit
		CleanupExit();
	case'p':
		// Toggle pause
		g_bPause = !g_bPause;
		break;
	case 'd':
		// Toggle drawing of the depth map
		g_bDrawDepthMap = !g_bDrawDepthMap;
		g_pDrawer->SetDepthMap(g_bDrawDepthMap);
		break;
	case 'f':
		g_bPrintFrameID = !g_bPrintFrameID;
		g_pDrawer->SetFrameID(g_bPrintFrameID);
		break;
	case 's':
		// Toggle smoothing
		if (g_fSmoothing == 0)
			g_fSmoothing = 0.1;
		else 
			g_fSmoothing = 0;
		g_HandsGenerator.SetSmoothing(g_fSmoothing);
		break;
	case 'e':
		// end current session
		g_pSessionManager->EndSession();
		break;
	}
}
void glInit (int * pargc, char ** argv)
{
	glutInit(pargc, argv);
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH);
	glutInitWindowSize(GL_WIN_SIZE_X, GL_WIN_SIZE_Y);
	glutCreateWindow ("PrimeSense Nite Point Viewer");
	//glutFullScreen();
	glutSetCursor(GLUT_CURSOR_NONE);

	glutKeyboardFunc(glutKeyboard);
	glutDisplayFunc(glutDisplay);
	glutIdleFunc(glutIdle);

	glDisable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);

	glEnableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
}
#endif


/*DJ's Functions*/
void togglePlay()
{

	StereoPlayer::IAutomationPtr pApp(__uuidof(StereoPlayer::Automation));
	
	ready = false;

	/*verify the player is ready by checking; note VT_BOOL=-1 is true; VT_BOOL=0 is false*/
	pApp->GetReady(&vResult);

	/*only perform the action if the player is ready*/
	if(vResult.boolVal == -1)
	{
		if(play)
		{
			pApp->SetPlaybackState(StereoPlayer::PlaybackState_Pause);
			play = false;
			pause = true;
			stop = false;
		}
		else if(pause | stop)
		{
			pApp->SetPlaybackState(StereoPlayer::PlaybackState_Play);
			pause = false;
			stop = false;
			play = true;
		}
	}
}

void setStop()
{
	StereoPlayer::IAutomationPtr pApp(__uuidof(StereoPlayer::Automation));
	/*verify the player is ready by checking; note VT_BOOL=-1 is true; VT_BOOL=0 is false*/
	pApp->GetReady(&vResult);

	/*only perform the action if the player is ready*/
	if(vResult.boolVal == -1)
	{
		pApp->SetPlaybackState(StereoPlayer::PlaybackState_Stop);
	}
}

void toggleScreen()
{
	StereoPlayer::IAutomationPtr pApp(__uuidof(StereoPlayer::Automation));
	
	/*verify the player is ready by checking; note VT_BOOL=-1 is true; VT_BOOL=0 is false*/
	pApp->GetReady(&vResult);

	/*only perform the action if the player is ready*/
	if(vResult.boolVal == -1)
	{
		if(!fullScreen)
		{
			pApp->EnterFullscreenMode(vBoolTrue);
			fullScreen = true;
		}
		else
		{
			pApp->LeaveFullscreenMode();
			fullScreen = false;
		}
	}
}

void increaseZoom()
{
	StereoPlayer::IAutomationPtr pApp(__uuidof(StereoPlayer::Automation));

	double tempZoom;
	double newZoom;
	pApp->GetZoom(&vResult);
	tempZoom = vResult.dblVal;

	printf("tempZoom = %g\n",tempZoom);
	

	/*verifying that the player is ready to receive a command*/
	pApp->GetReady(&vResult);

	/*only perform these commands if the player is ready*/
	if (vResult.boolVal == -1)
	{

		if (zoom==tempZoom)
		{
			newZoom = zoom + 10.0;
			pApp->SetZoom(newZoom);
			zoom = newZoom;
		}
		else
		{
			if(print_debug)
			{
				cout <<"Warning: The zoom setting is not consistently set. Check the initial settings." << endl;
			}
			zoom = tempZoom;
			newZoom = zoom + 10.0;
			pApp->SetZoom(newZoom);
			zoom = newZoom;
		}
	}
}

void decreaseZoom()
{
	StereoPlayer::IAutomationPtr pApp(__uuidof(StereoPlayer::Automation));

	double tempZoom;
	double newZoom;
	pApp->GetZoom(&vResult);
	tempZoom = vResult.dblVal;


	/*verify the player is ready by checking; note VT_BOOL=-1 is true; VT_BOOL=0 is false*/
	pApp->GetReady(&vResult);

	/*only perform the action if the player is ready*/
	if(vResult.boolVal == -1)
	{
		if (zoom==tempZoom)
		{
			newZoom = zoom - 10.0;
			pApp->SetZoom(newZoom);
			OutputDebugString(TEXT("NEW ZOOM LEVEL: %g\n",newZoom));
			zoom = newZoom;
		}
		else
		{
			if(print_debug)
			{
				cout <<"Warning: The zoom setting is not consistently set. Check the initial settings." << endl;
			}
			zoom = tempZoom;
			newZoom = zoom - 10.0;
			pApp->SetZoom(newZoom);
			zoom = newZoom;
		}
	}
}

void getPlaybackState()
{
	StereoPlayer::IAutomationPtr pApp(__uuidof(StereoPlayer::Automation));
	hr = pApp->GetPlaybackState(&vResult);

	cout << "Playback State: " << vResult.dblVal << endl;


}

/*End DJ's Functions*/


void XN_CALLBACK_TYPE GestureIntermediateStageCompletedHandler(xn::GestureGenerator& generator, const XnChar* strGesture, const XnPoint3D* pPosition, void* pCookie)
{
	if(print_debug)
		printf("Gesture %s: Intermediate stage complete (%f,%f,%f)\n", strGesture, pPosition->X, pPosition->Y, pPosition->Z);
}

void XN_CALLBACK_TYPE GestureReadyForNextIntermediateStageHandler(xn::GestureGenerator& generator, const XnChar* strGesture, const XnPoint3D* pPosition, void* pCookie)
{
	if(print_debug)
		printf("Gesture %s: Ready for next intermediate stage (%f,%f,%f)\n", strGesture, pPosition->X, pPosition->Y, pPosition->Z);
}

void XN_CALLBACK_TYPE GestureProgressHandler(xn::GestureGenerator& generator, const XnChar* strGesture, const XnPoint3D* pPosition, XnFloat fProgress, void* pCookie)
{
	if(print_debug)
		printf("Gesture %s progress: %f (%f,%f,%f)\n", strGesture, fProgress, pPosition->X, pPosition->Y, pPosition->Z);
}

void XN_CALLBACK_TYPE CircleCB(XnFloat fTimes, XnBool bConfident, const XnVCircle* pCircle, void* pUserCxt)
{
	printf("\nCircle Detected\n");
	g_pCircle->Reset();

	if (circleCount == 0)
	{
		sliderMode = true;
		commandState = 1;
		if(print_debug)
		{
			cout << "Slide Mode activated." << endl;
		}

		//activate progress sliders here
		
		circleCount ++;
	}


	//you must make the circle gesture twice to exit
	else if (circleCount ==1)
	{
		commandState = 5;
		sliderMode= true;
		CleanupExit();
		circleCount = 0;
	}
	
}

void XN_CALLBACK_TYPE NoCircleCB(XnFloat fLastValue, XnVCircleDetector::XnVNoCircleReason eReason, void* pUserCxt)
{
	//nothing goes here
}

void XN_CALLBACK_TYPE SwipeDownCB(XnFloat fVelocity, XnFloat fAngle, void* pUserCxt)
{
	if(!sliderMode)
	{
		printf("\nSwipe Down -- DECREASE ZOOM\n");
		decreaseZoom();
	}
}

void XN_CALLBACK_TYPE SwipeUpCB(XnFloat fVelocity, XnFloat fAngle, void* pUserCxt)
{
	if(!sliderMode)
	{
		printf("\nSwipe Up -- INCREASE ZOOM\n");
		increaseZoom();
	}
}

void XN_CALLBACK_TYPE SwipeLeftCB(XnFloat fVelocity, XnFloat fAngle, void* pUserCxt)
{
	if(!sliderMode)
	{
		printf("\n Left Swipe -- SWITCH FULLSCREEN STATE\n");
		toggleScreen();
	}
}

void XN_CALLBACK_TYPE SwipeRightCB(XnFloat fVelocity, XnFloat fAngle, void* pUserCxt)
{
	
	
	if(!sliderMode)
	{
		printf("\nSwipe Right -- TOGGLE PLAY/PAUSE\n");
		togglePlay();
	}
}

void XN_CALLBACK_TYPE WaveCB(void* pUserCxt)
{
	printf("\nWave Detected\n");
}

void XN_CALLBACK_TYPE PushCB(XnFloat fVelocity, XnFloat fAngle, void* UserCxt)
{

	if(!sliderMode)
	{
		printf("\nPush Detected -- STOP PLAYBACK\n");
		setStop();
	}

	if(sliderMode)
	{
		printf("\nPush Detected -- SET PLAYBACK POSITION\n");
		StereoPlayer::IAutomationPtr pApp(__uuidof(StereoPlayer::Automation));

		pApp->SetPosition((double)playbackPosition*duration);
		pApp->SetPlaybackState(StereoPlayer::PlaybackState_Play);
		sliderMode = false;
		circleCount = 0;
	}
}

void XN_CALLBACK_TYPE SlideCB(XnFloat fValue, void* pUserCxt)
{
	if(sliderMode)
	{
		printf("\tPosition - %g\n",fValue);
		playbackPosition = fValue;
	}

}

//sample XML code that will initialize the OpenNI interface
#define SAMPLE_XML_PATH "Sample-Tracking.xml"

int main(int argc, char** argv)
{
	//error handling variables
	XnStatus rc = XN_STATUS_OK;
	xn::EnumerationErrors errors;

	play = false;
	pause = false;
	stop = false;
	fullScreen = false;
	zoom = 100;
	circleCount = 0;
	sliderMode = false;
	commandState = 2;

	hr = CoInitialize(NULL);


	if FAILED(hr)
	{
		cout << "Error" << endl;
	}
	StereoPlayer::IAutomationPtr app(__uuidof(StereoPlayer::Automation));


	ready = false;

	while(!ready)
	{
		app->GetReady(&vResult);
		
		if(vResult.boolVal == -1 )
		{
			ready = true;
		}
		else if (vResult.boolVal == 0)
			ready = false;
	}


	hr = app->OpenFile(L"C:\\Users\\Public\\Videos\\Pulmonary.mov");
	if FAILED(hr)
		goto error;
	hr = app->SetPlaybackState(StereoPlayer::PlaybackState_Play);
	play = true;
	if FAILED(hr)
		goto error;

	hr = app->GetDuration(&vResult);
	if FAILED(hr)
		goto error;
	duration = vResult.dblVal;
	printf("Duration: %g seconds\n",duration);

	if (duration >= 60)
	{
		if(print_debug)
		{
			cout << "Video duration is greater than 60 seconds. Turning off repeat..." << endl;
		}
		app->SetRepeat(vBoolFalse);
	}
	else
	{
		if(print_debug)
		{
			cout << "Video duration less than 60 seconds. Turning on repeat..." << endl;
		}
		app->SetRepeat(vBoolTrue);
	}

	commandState = 0;

	//Initialize the OpenNI interface to the Kinect Camera
	rc = g_Context.InitFromXmlFile(SAMPLE_XML_PATH, g_ScriptNode,&errors);
	CHECK_ERRORS(rc,errors,"InitFromXMLFile");
	CHECK_RC(rc,"InitFromXMLFile");

	rc=g_Context.FindExistingNode(XN_NODE_TYPE_DEPTH, g_DepthGenerator);
	CHECK_RC(rc,"Find depth generator");

	rc= g_Context.FindExistingNode(XN_NODE_TYPE_HANDS,g_HandsGenerator);
	CHECK_RC(rc,"Find Hands Generator");

	rc=g_Context.FindExistingNode(XN_NODE_TYPE_GESTURE,g_GestureGenerator);
	CHECK_RC(rc,"Find Gesture Generator");

	XnCallbackHandle h;

	//if we've activating the sensor for knowing whether or not the hand is near the FOV edge, register the callback
	if (g_HandsGenerator.IsCapabilitySupported(XN_CAPABILITY_HAND_TOUCHING_FOV_EDGE))
	{
		g_HandsGenerator.GetHandTouchingFOVEdgeCap().RegisterToHandTouchingFOVEdge(TouchingCallback, NULL,h);
	}

	XnCallbackHandle hGestureIntermediateStageCompleted, hGestureProgress, hGestureReadyForNextIntermediateStage;
	g_GestureGenerator.RegisterToGestureIntermediateStageCompleted(GestureIntermediateStageCompletedHandler, NULL, hGestureIntermediateStageCompleted);
	g_GestureGenerator.RegisterToGestureReadyForNextIntermediateStage(GestureReadyForNextIntermediateStageHandler, NULL, hGestureReadyForNextIntermediateStage);
	g_GestureGenerator.RegisterGestureCallbacks(NULL, GestureProgressHandler, NULL, hGestureProgress);

	//We already created OpenNI objects, now we need to Create NITE objects

	g_pSessionManager = new XnVSessionManager;
	rc = g_pSessionManager->Initialize(&g_Context,"Click,Wave","RaiseHand");
	CHECK_RC(rc, "SessionManager Initialization");

	g_pSessionManager->RegisterSession(NULL,SessionStarting,SessionEnding,FocusProgress);

	g_pDrawer = new XnVPointDrawer(20, g_DepthGenerator);
	g_pFlowRouter = new XnVFlowRouter;
	g_pFlowRouter->SetActive(g_pDrawer);

	//will now draw the circle on the hand
	g_pSessionManager->AddListener(g_pFlowRouter);

	//logic and registration for the circle detector
	g_pCircle = new XnVCircleDetector;
	g_pCircle->RegisterCircle(NULL,&CircleCB); //when circle is recognized, CircleCB will be called
	g_pCircle->RegisterNoCircle(NULL,&NoCircleCB); //when circle stops being detector, NoCircleCB will be called
	
	//UNCOMMENT TO ENABLE THE CIRCLE DETECTOR
	g_pSessionManager->AddListener(g_pCircle);

	//logic and registration for the swipe detector and its 4 events
	g_pSwipe = new XnVSwipeDetector;

	//change default settings to optimize the gesture recognition accuracy
	g_pSwipe->SetSteadyDuration(500);
	g_pSwipe->SetMotionSpeedThreshold(0.20);
	g_pSwipe->SetMotionTime(500);
	g_pSwipe->SetXAngleThreshold(45);
	g_pSwipe->SetYAngleThreshold(45);



	g_pSwipe->RegisterSwipeDown(NULL, &SwipeDownCB);
	g_pSwipe->RegisterSwipeLeft(NULL, &SwipeLeftCB);
	g_pSwipe->RegisterSwipeRight(NULL, &SwipeRightCB);
	g_pSwipe->RegisterSwipeUp(NULL, &SwipeUpCB);

	g_pSessionManager->AddListener(g_pSwipe);

	//registration for wave detector
	g_pWave = new XnVWaveDetector;
	g_pWave->RegisterWave(NULL, &WaveCB);

	g_pSessionManager->AddListener(g_pWave);

	//register for the push detector
	g_pPush = new XnVPushDetector;
	g_pPush->RegisterPush(NULL, &PushCB);

	g_pSessionManager->AddListener(g_pPush);
	
	//register for the slider event
	g_pSlider = new XnVSelectableSlider1D(1,5,AXIS_X,0,0.5,250);
	g_pSlider->RegisterValueChange(NULL, SlideCB);

	g_pSessionManager->AddListener(g_pSlider);


	g_pDrawer->RegisterNoPoints(NULL, NoHands);
	g_pDrawer->SetDepthMap(g_bDrawDepthMap);

	rc = g_Context.StartGeneratingAll();
	CHECK_RC(rc,"Start Generating");



#ifdef USE_GLUT

	glInit(&argc, argv);
	glutMainLoop();

#elif defined(USE_GLES)
	if (!opengles_init(GL_WIN_SIZE_X, GL_WIN_SIZE_Y, &display, &surface, &context))
	{
		printf("Error initializing opengles\n");
		CleanupExit();
	}
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
	glEnableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);

	while ((!_kbhit()) && (!g_bQuit))
	{
		glutDisplay();
		eglSwapBuffers(display, surface);
	}
	opengles_shutdown(display, surface, context);

	CleanupExit();
#endif

	cout << "Please press a key" << endl;
	int temp;
	cin >> temp;
error:
    return hr;
}