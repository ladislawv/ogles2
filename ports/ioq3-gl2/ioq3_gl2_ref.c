/*
 * ports/ioq3-gl2: upstream renderergl2 in the Amiga engine (Cowcat's
 * ioquake3). Both use REF_API_VERSION 8 and the same refexport_t and shared
 * types (tr_types.h), but upstream's refimport_t has ten more members. The
 * engine calls GetRefAPI with its own refimport_t; this copies it into
 * upstream's, adds the rest, and calls the renderer's (renamed
 * GL2_GetRefAPI when it is compiled). Also the one upstream q_math.c
 * function the renderer needs that the engine lacks.
 *
 * GPL v2 or later, as ioquake3.
 */
#define GetRefAPI GL2_GetRefAPI
#include "../renderergl2/tr_local.h"
#undef GetRefAPI

/* The engine's refimport_t (its code/renderer/tr_public.h), field by field. */
typedef struct {
	void (QDECL *Printf)(int printLevel, const char *fmt, ...);
	void (QDECL *Error)(int errorLevel, const char *fmt, ...);
	int (*Milliseconds)(void);
	void *(*Hunk_Alloc)(int size, ha_pref pref);
	void *(*Hunk_AllocateTempMemory)(int size);
	void (*Hunk_FreeTempMemory)(void *block);
	void *(*Malloc)(int bytes);
	void (*Free)(void *buf);
	cvar_t *(*Cvar_Get)(const char *name, const char *value, int flags);
	void (*Cvar_Set)(const char *name, const char *value);
	void (*Cvar_CheckRange)(cvar_t *cv, float minVal, float maxVal, qboolean shouldBeIntegral);
	void (*Cmd_AddCommand)(const char *name, void (*cmd)(void));
	void (*Cmd_RemoveCommand)(const char *name);
	int (*Cmd_Argc)(void);
	char *(*Cmd_Argv)(int i);
	void (*Cmd_ExecuteText)(int exec_when, const char *text);
	void (*CM_DrawDebugSurface)(void (*drawPoly)(int color, int numPoints, float *points));
	int (*FS_FileIsInPAK)(const char *name, int *pCheckSum);
	long (*FS_ReadFile)(const char *name, void **buf);
	void (*FS_FreeFile)(void *buf);
	char **(*FS_ListFiles)(const char *name, const char *extension, int *numfilesfound);
	void (*FS_FreeFileList)(char **filelist);
	void (*FS_WriteFile)(const char *qpath, const void *buffer, int size);
	qboolean (*FS_FileExists)(const char *file);
	void (*CIN_UploadCinematic)(int handle);
	int (*CIN_PlayCinematic)(const char *arg0, int xpos, int ypos, int width, int height, int bits);
	e_status (*CIN_RunCinematic)(int handle);
	void (*CL_WriteAVIVideoFrame)(const byte *buffer, int size);
} engine_refimport_t;

/* The engine's (qcommon, code/amiga). */
void Cvar_SetValue(const char *var_name, float value);
int Cvar_VariableIntegerValue(const char *var_name);
byte *CM_ClusterPVS(int cluster);
void IN_Init(void);
void IN_Shutdown(void);
void IN_Restart(void);

static void Cvar_SetDescription_none(cvar_t *cv, const char *description)
{
	/* the engine's cvars have no description */
}

static void IN_Init_amiga(void *windowData)
{
	IN_Init();
}

static long ftol_c(float f)
{
	return (long)f;
}

static void Sys_SetEnv_none(const char *name, const char *value)
{
}

static void Sys_GLimp_none(void)
{
}

static qboolean Sys_LowPhysicalMemory_no(void)
{
	return qfalse;
}

/* Upstream's q_math.c has it, the engine's does not (tr_sky.c). */
float Q_acos(float c)
{
	float angle = acos(c);
	if (angle > M_PI)
		return (float)M_PI;
	if (angle < -M_PI)
		return (float)M_PI;
	return angle;
}

static refimport_t up;

refexport_t *GetRefAPI(int apiVersion, engine_refimport_t *e)
{
	Com_Memset(&up, 0, sizeof(up));
	up.Printf = e->Printf;
	up.Error = (__typeof__(up.Error))e->Error; /* noreturn upstream: Com_Error is */
	up.Milliseconds = e->Milliseconds;
	up.Hunk_Alloc = e->Hunk_Alloc;
	up.Hunk_AllocateTempMemory = e->Hunk_AllocateTempMemory;
	up.Hunk_FreeTempMemory = e->Hunk_FreeTempMemory;
	up.Malloc = e->Malloc;
	up.Free = e->Free;
	up.Cvar_Get = e->Cvar_Get;
	up.Cvar_Set = e->Cvar_Set;
	up.Cvar_SetValue = Cvar_SetValue;
	up.Cvar_CheckRange = e->Cvar_CheckRange;
	up.Cvar_SetDescription = Cvar_SetDescription_none;
	up.Cvar_VariableIntegerValue = Cvar_VariableIntegerValue;
	up.Cmd_AddCommand = e->Cmd_AddCommand;
	up.Cmd_RemoveCommand = e->Cmd_RemoveCommand;
	up.Cmd_Argc = e->Cmd_Argc;
	up.Cmd_Argv = e->Cmd_Argv;
	up.Cmd_ExecuteText = e->Cmd_ExecuteText;
	up.CM_ClusterPVS = CM_ClusterPVS;
	up.CM_DrawDebugSurface = e->CM_DrawDebugSurface;
	up.FS_FileIsInPAK = e->FS_FileIsInPAK;
	up.FS_ReadFile = e->FS_ReadFile;
	up.FS_FreeFile = e->FS_FreeFile;
	up.FS_ListFiles = e->FS_ListFiles;
	up.FS_FreeFileList = e->FS_FreeFileList;
	up.FS_WriteFile = e->FS_WriteFile;
	up.FS_FileExists = e->FS_FileExists;
	up.CIN_UploadCinematic = e->CIN_UploadCinematic;
	up.CIN_PlayCinematic = e->CIN_PlayCinematic;
	up.CIN_RunCinematic = e->CIN_RunCinematic;
	up.CL_WriteAVIVideoFrame = e->CL_WriteAVIVideoFrame;
	up.IN_Init = IN_Init_amiga;
	up.IN_Shutdown = IN_Shutdown;
	up.IN_Restart = IN_Restart;
	up.ftol = ftol_c;
	up.Sys_SetEnv = Sys_SetEnv_none;
	up.Sys_GLimpSafeInit = Sys_GLimp_none;
	up.Sys_GLimpInit = Sys_GLimp_none;
	up.Sys_LowPhysicalMemory = Sys_LowPhysicalMemory_no;
	return GL2_GetRefAPI(apiVersion, &up);
}
