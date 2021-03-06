#include <thread>
#include <windows.h>
#include <math.h>
#include <atlstr.h> 
#include "IniReader\IniReader.h"

#define DLLEXPORT extern "C" __declspec(dllexport)

#define XINPUT_GAMEPAD_DPAD_UP          0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN        0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT        0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT       0x0008
#define XINPUT_GAMEPAD_START            0x0010
#define XINPUT_GAMEPAD_BACK             0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB       0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB      0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER    0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER   0x0200
#define XINPUT_GAMEPAD_A                0x1000
#define XINPUT_GAMEPAD_B                0x2000
#define XINPUT_GAMEPAD_X                0x4000
#define XINPUT_GAMEPAD_Y				0x8000

#define BATTERY_TYPE_DISCONNECTED		0x00

#define XUSER_MAX_COUNT                 4
#define XUSER_INDEX_ANY					0x000000FF

#define ERROR_DEVICE_NOT_CONNECTED		1167
#define ERROR_SUCCESS					0

//
// Structures used by XInput APIs
//
typedef struct _XINPUT_GAMEPAD
{
	WORD                                wButtons;
	BYTE                                bLeftTrigger;
	BYTE                                bRightTrigger;
	SHORT                               sThumbLX;
	SHORT                               sThumbLY;
	SHORT                               sThumbRX;
	SHORT                               sThumbRY;
} XINPUT_GAMEPAD, *PXINPUT_GAMEPAD;

typedef struct _XINPUT_STATE
{
	DWORD                               dwPacketNumber;
	XINPUT_GAMEPAD                      Gamepad;
} XINPUT_STATE, *PXINPUT_STATE;

typedef struct _XINPUT_VIBRATION
{
	WORD                                wLeftMotorSpeed;
	WORD                                wRightMotorSpeed;
} XINPUT_VIBRATION, *PXINPUT_VIBRATION;

typedef struct _XINPUT_CAPABILITIES
{
	BYTE                                Type;
	BYTE                                SubType;
	WORD                                Flags;
	XINPUT_GAMEPAD                      Gamepad;
	XINPUT_VIBRATION                    Vibration;
} XINPUT_CAPABILITIES, *PXINPUT_CAPABILITIES;

typedef struct _XINPUT_BATTERY_INFORMATION
{
	BYTE BatteryType;
	BYTE BatteryLevel;
} XINPUT_BATTERY_INFORMATION, *PXINPUT_BATTERY_INFORMATION;

typedef struct _XINPUT_KEYSTROKE
{
	WORD    VirtualKey;
	WCHAR   Unicode;
	WORD    Flags;
	BYTE    UserIndex;
	BYTE    HidCode;
} XINPUT_KEYSTROKE, *PXINPUT_KEYSTROKE;

typedef DWORD(__stdcall *_XInputGetState)(_In_ DWORD dwUserIndex, _Out_ XINPUT_STATE *pState);
typedef DWORD(__stdcall *_XInputSetState)(_In_ DWORD dwUserIndex, _In_ XINPUT_VIBRATION *pVibration);

_XInputGetState MyXInputGetState;
_XInputSetState MyXInputSetState;

HMODULE hDll;

#define FREETRACK_HEAP "FT_SharedMem"
#define FREETRACK_MUTEX "FT_Mutext"

/* only 6 headpose floats and the data id are filled -sh */
typedef struct FTData__ {
	uint32_t DataID;
	int32_t CamWidth;
	int32_t CamHeight;
	/* virtual pose */
	float  Yaw;   /* positive yaw to the left */
	float  Pitch; /* positive pitch up */
	float  Roll;  /* positive roll to the left */
	float  X;
	float  Y;
	float  Z;
	/* raw pose with no smoothing, sensitivity, response curve etc. */
	float  RawYaw;
	float  RawPitch;
	float  RawRoll;
	float  RawX;
	float  RawY;
	float  RawZ;
	/* raw points, sorted by Y, origin top left corner */
	float  X1;
	float  Y1;
	float  X2;
	float  Y2;
	float  X3;
	float  Y3;
	float  X4;
	float  Y4;
} volatile FTData;

typedef struct FTHeap__ {
	FTData data;
	int32_t GameID;
	union
	{
		unsigned char table[8];
		int32_t table_ints[2];
	};
	int32_t GameID2;
} volatile FTHeap;

static HANDLE hFTMemMap = 0;
static FTHeap *ipc_heap = 0;
static HANDLE ipc_mutex = 0;

FTData *FreeTrack;
bool FTThInit = false, FTThWork = false;
std::thread *pFTthread = NULL;

//FreeTrack implementation from OpenTrack (https://github.com/opentrack/opentrack/tree/unstable/freetrackclient)
static BOOL impl_create_mapping(void)
{
	if (ipc_heap != NULL)
		return TRUE;

	hFTMemMap = CreateFileMappingA(INVALID_HANDLE_VALUE,
		NULL,
		PAGE_READWRITE,
		0,
		sizeof(FTHeap),
		(LPCSTR)FREETRACK_HEAP);

	if (hFTMemMap == NULL)
		return (ipc_heap = NULL), FALSE;

	ipc_heap = (FTHeap*)MapViewOfFile(hFTMemMap, FILE_MAP_WRITE, 0, 0, sizeof(FTHeap));
	ipc_mutex = CreateMutexA(NULL, FALSE, FREETRACK_MUTEX);

	return TRUE;
}

void FTRead()
{
	while (FTThWork) {
		if (ipc_mutex && WaitForSingleObject(ipc_mutex, 16) == WAIT_OBJECT_0) {
			memcpy(&FreeTrack, &ipc_heap, sizeof(FreeTrack));
			if (ipc_heap->data.DataID > (1 << 29))
				ipc_heap->data.DataID = 0;
			ReleaseMutex(ipc_mutex);
		}
	}
}

double RadToDeg(float r) {
	return r * (180 / 3.14159265358979323846); //180 / PI
}

BYTE GameMode = 0;
int WheelAngle;
double RThumbSensX, RThumbSensY; 
_XINPUT_STATE myPState;

SHORT ToLeftStick(double Data)
{
	int MyData = round((32767 / WheelAngle) * Data); 
	if (MyData < -32767) MyData = -32767;
	if (MyData > 32767) MyData = 32767;
	return MyData;
}

SHORT ThumbFix(double Data)
{
	int MyData = round(Data);
	if (MyData > 32767) MyData = 32767;
	if (MyData < -32767) MyData = -32767;
	return MyData;
}

DLLEXPORT BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	//case DLL_PROCESS_ATTACH:
	//case DLL_THREAD_ATTACH:
	//case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		if (hDll != NULL) {
			FreeLibrary(hDll);
			hDll = nullptr;
		}
		if (FTThWork) {
			FTThWork = false;
			if (pFTthread) {
				pFTthread->join();
				delete pFTthread;
				pFTthread = nullptr;
			}
		}
		break;
	}
	return TRUE;
}

DLLEXPORT DWORD WINAPI XInputGetState(_In_ DWORD dwUserIndex, _Out_ XINPUT_STATE *pState)
{
	if (FTThInit == false) {
		FTThInit = true;

		CIniReader IniFile("X360Advance.ini");
		WheelAngle = IniFile.ReadFloat("Main", "WheelAngle", 80);
		RThumbSensX = IniFile.ReadFloat("Main", "RThumbSensX", -1);
		RThumbSensY = IniFile.ReadFloat("Main", "RThumbSensY", -1);

		hDll = LoadLibrary(_T("C:\\Windows\\System32\\xinput1_3.dll"));

		if (hDll != NULL) {

			MyXInputGetState = (_XInputGetState)GetProcAddress(hDll, "XInputGetState");
			MyXInputSetState = (_XInputSetState)GetProcAddress(hDll, "XInputSetState");

			if (MyXInputGetState == NULL || MyXInputSetState == NULL)
				hDll = NULL;

		}

		if (impl_create_mapping() == false) {
			FTThWork = false;
		}
		else {
			FTThWork = true;
			pFTthread = new std::thread(FTRead);
		}
	}
	
	pState->Gamepad.wButtons = 0;
	pState->Gamepad.bRightTrigger = 0;
	pState->Gamepad.bLeftTrigger = 0;
	pState->Gamepad.sThumbLX = 0;
	pState->Gamepad.sThumbLY = 0;
	pState->Gamepad.sThumbRX = 0;
	pState->Gamepad.sThumbRY = 0;

	if ((GetAsyncKeyState(VK_F9) & 0x8000) != 0) GameMode = 1; //FPS
	if ((GetAsyncKeyState(VK_F10) & 0x8000) != 0) GameMode = 2; //Wheel
	if ((GetAsyncKeyState(VK_F11) & 0x8000) != 0) GameMode = 0; //Default

	DWORD myStatus = ERROR_DEVICE_NOT_CONNECTED;
	if (hDll != NULL)
		myStatus = MyXInputGetState(dwUserIndex, &myPState);
	
	if (myStatus == ERROR_SUCCESS) {
		pState->Gamepad.bRightTrigger = myPState.Gamepad.bRightTrigger;
		pState->Gamepad.bLeftTrigger = myPState.Gamepad.bLeftTrigger;
		pState->Gamepad.wButtons = myPState.Gamepad.wButtons;

		//FPS
		if ((myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
			&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
			&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)) GameMode = 1;

		//Wheel
		if ((myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
		&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
		&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_START)) GameMode = 2;

		//Default
		if ((myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
			&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
			&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)
			&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_START)) GameMode = 0;

		//�� ����� �������� ������ � ���� ��� ����� ������
		if (((myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
			&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
			&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)) ||

			((myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
				&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
				&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_START)) ||

				((myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
					&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
					&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)
					&& (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_START))
			) pState->Gamepad.wButtons = 0;

		switch (GameMode)
		{
		case 0: //Default
			{
				pState->Gamepad.sThumbLX = myPState.Gamepad.sThumbLX;
				pState->Gamepad.sThumbLY = myPState.Gamepad.sThumbLY;
				pState->Gamepad.sThumbRX = myPState.Gamepad.sThumbRX;
				pState->Gamepad.sThumbRY = myPState.Gamepad.sThumbRY;
			}
			break;
		case 1: //FPS
			{
				pState->Gamepad.sThumbLX = myPState.Gamepad.sThumbLX;
				pState->Gamepad.sThumbLY = myPState.Gamepad.sThumbLY;
				pState->Gamepad.sThumbRX = ThumbFix(myPState.Gamepad.sThumbRX + RadToDeg(FreeTrack->Pitch) * RThumbSensX);
				pState->Gamepad.sThumbRY = ThumbFix(myPState.Gamepad.sThumbRY + RadToDeg(FreeTrack->Roll) * RThumbSensY);
			}
			break;
		case 2:	//Wheel
			{
				pState->Gamepad.sThumbLX = ToLeftStick(RadToDeg(FreeTrack->Pitch)* -1);
				pState->Gamepad.sThumbLY = myPState.Gamepad.sThumbLY;
				pState->Gamepad.sThumbRX = myPState.Gamepad.sThumbRX;
				pState->Gamepad.sThumbRY = myPState.Gamepad.sThumbRY;
			}
			break;
		}
	}


	pState->dwPacketNumber = GetTickCount();

	return myStatus;
}

DLLEXPORT DWORD WINAPI XInputSetState(_In_ DWORD dwUserIndex, _In_ XINPUT_VIBRATION *pVibration) 
{
	DWORD myStatus = ERROR_DEVICE_NOT_CONNECTED;
	if (hDll != NULL)
		myStatus = MyXInputSetState(dwUserIndex, pVibration);
	return myStatus;
}


DLLEXPORT DWORD WINAPI XInputGetCapabilities(_In_ DWORD dwUserIndex, _In_ DWORD dwFlags, _Out_ XINPUT_CAPABILITIES *pCapabilities) 
{
	if (dwUserIndex == 0) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT void WINAPI XInputEnable(_In_ BOOL enable)
{

}

DLLEXPORT DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID* pDSoundRenderGuid, GUID* pDSoundCaptureGuid)
{
	if (dwUserIndex == 0) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD WINAPI XInputGetBatteryInformation(_In_ DWORD dwUserIndex, _In_ BYTE devType, _Out_ XINPUT_BATTERY_INFORMATION *pBatteryInformation)
{
	if (dwUserIndex == 0) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, PXINPUT_KEYSTROKE pKeystroke)
{
	if (dwUserIndex == 0) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD WINAPI XInputGetStateEx(_In_ DWORD dwUserIndex, _Out_ XINPUT_STATE *pState)
{
	if (dwUserIndex == 0) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD WINAPI XInputWaitForGuideButton(_In_ DWORD dwUserIndex, _In_ DWORD dwFlag, _In_ LPVOID pVoid)
{
	if (dwUserIndex == 0) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD XInputCancelGuideButtonWait(_In_ DWORD dwUserIndex)
{
	if (dwUserIndex == 0) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}

DLLEXPORT DWORD XInputPowerOffController(_In_ DWORD dwUserIndex)
{
	if (dwUserIndex == 0) {
		return ERROR_SUCCESS;
	}
	else
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
}