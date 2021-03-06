/*
    CGMI
    Copyright (C) {2015}  {Cisco System}

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
    USA

    Contributing Authors: Matt Snoby, Kris Kersey, Zack Wine, Chris Foster,
                          Tankut Akgul, Saravanakumar Periyaswamy

*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Includes */
#include <glib.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <pthread.h>

// put this in a define. #include "diaglib.h"
#include <sys/time.h>

#include "cgmiPlayerApi.h"
#include "cgmiDiagsApi.h"

#ifdef TMET_ENABLED
#include "http-timing-metrics.h"
#endif // TMET_ENABLED


/* Defines for section filtering. */
#define MAX_PMT_COUNT 20

#define MAX_COMMAND_LENGTH 1024
#define MAX_FILTER_LENGTH 256
#define MAX_VALUE_LENGTH 208
#define MAX_HISTORY 50

static void *filterid = NULL;
static bool filterRunning = false;
static struct termios oldt, newt;
static int gAutoPlay = true;
static char *gCurrentPlaySrcUrl = NULL;
#ifdef TMET_ENABLED
static char *gDefaultPostUrl = NULL;
#endif // TMET_ENABLED
static pthread_mutex_t cgmiCliMutex = PTHREAD_MUTEX_INITIALIZER;
void crash (void)
{
   volatile int *a = (int*)(NULL);
   *a = 1;
}
/* Prototypes */
static void cgmiCallback( void *pUserData, void *pSession, tcgmi_Event event, uint64_t code );
static cgmi_Status destroyfilter( void *pSessionId );
static void dumpTimingEntry(void);
static void updateCurrentPlaySrcUrl(char *src);

/* Signal Handler */
void sig_handler(int signum)
{
    tcsetattr( STDIN_FILENO, TCSANOW, &oldt);
    printf("\n\nNext time you may want to use Ctrl+D to exit correctly. :-)\n");
    exit(1);
}


/*Dumping timing entry*/
static void dumpTimingEntry(void)
{
    int maxCount, i, numEntry;
    tCgmiDiags_timingMetric *pMetricsBuf = NULL;

    if(CGMI_ERROR_SUCCESS == cgmiDiags_GetTimingMetricsMaxCount(&maxCount))
    {
        pMetricsBuf = (tCgmiDiags_timingMetric *)malloc(sizeof(tCgmiDiags_timingMetric)*maxCount);
    }

    if(NULL != pMetricsBuf)
    {
        numEntry = maxCount;
        if(CGMI_ERROR_SUCCESS == cgmiDiags_GetTimingMetrics (pMetricsBuf, &numEntry))
        {
            for(i=0;(i < maxCount) && (i < numEntry);i++)
            {
                switch(pMetricsBuf[i].timingEvent)
                {
                    case DIAG_TIMING_METRIC_UNLOAD:
                    {
                        printf("event = DIAG_TIMING_METRIC_UNLOAD; index = %d; time = %llu; uri = %s\n", pMetricsBuf[i].sessionIndex, pMetricsBuf[i].markTime, pMetricsBuf[i].sessionUri);
                    }
                    break;
                    case DIAG_TIMING_METRIC_LOAD:
                    {
                        printf("event = DIAG_TIMING_METRIC_LOAD; index = %d; time = %llu; uri = %s\n", pMetricsBuf[i].sessionIndex, pMetricsBuf[i].markTime, pMetricsBuf[i].sessionUri);
                    }
                    break;
                    case DIAG_TIMING_METRIC_PLAY:
                    {
                        printf("event = DIAG_TIMING_METRIC_PLAY; index = %d; time = %llu; uri = %s\n", pMetricsBuf[i].sessionIndex, pMetricsBuf[i].markTime, pMetricsBuf[i].sessionUri);
                    }
                    break;
                    case DIAG_TIMING_METRIC_PTS_DECODED:
                    {
                        printf("event = DIAG_TIMING_METRIC_PTS_DECODED; index = %d; time = %llu; uri = %s\n", pMetricsBuf[i].sessionIndex, pMetricsBuf[i].markTime, pMetricsBuf[i].sessionUri);
                    }
                    break;
                    default:
                        printf("Unknown entry!\n");
                    break;
                }
            }
        }
        free(pMetricsBuf);
    }
}

/*Dumping timing entry*/
static void dumpChannelChangeTime(void)
{
    int maxCount, i, numEntry;
    tCgmiDiags_timingMetric *pMetricsBuf = NULL;

    cgmiDiags_GetTimingMetricsMaxCount(&maxCount);

    if(CGMI_ERROR_SUCCESS == cgmiDiags_GetTimingMetricsMaxCount(&maxCount))
    {
        pMetricsBuf = (tCgmiDiags_timingMetric *)malloc(sizeof(tCgmiDiags_timingMetric)*maxCount);
    }

    if(NULL != pMetricsBuf)
    {
        numEntry = maxCount;
        if(CGMI_ERROR_SUCCESS == cgmiDiags_GetTimingMetrics (pMetricsBuf, &numEntry))
        {
            for(i=0;(i < maxCount) && (i < numEntry);i++)
            {
                switch(pMetricsBuf[i].timingEvent)
                {
                    int j;

                    case DIAG_TIMING_METRIC_LOAD:
                    {
                        for(j=i+1;(j < maxCount) && (j < numEntry);j++)
                        {
                            if((DIAG_TIMING_METRIC_PTS_DECODED == pMetricsBuf[j].timingEvent) && (pMetricsBuf[i].sessionIndex == pMetricsBuf[j].sessionIndex))
                            {
                                printf("Channel change time for index = %d is %llu ms with uri = %s\n", pMetricsBuf[i].sessionIndex, pMetricsBuf[j].markTime - pMetricsBuf[i].markTime, pMetricsBuf[i].sessionUri);
                            }
                        }
                    }
                    break;
                    default:
                        //nop
                    break;
                }
            }
        }
        free(pMetricsBuf);
    }
}

/*update current playing url*/
static void updateCurrentPlaySrcUrl(char *src)
{
    if(NULL != src)
    {
        pthread_mutex_lock(&cgmiCliMutex);

        if(gCurrentPlaySrcUrl)
        {
            free(gCurrentPlaySrcUrl);
            gCurrentPlaySrcUrl = NULL;
        }

        if(NULL != src)
        {
            gCurrentPlaySrcUrl = strdup(src);
        }

        pthread_mutex_unlock(&cgmiCliMutex);
    }
}

/* Load Command */
static cgmi_Status load(void *pSessionId, char *src, cpBlobStruct * cpblob, const char *sessionSettings)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

    updateCurrentPlaySrcUrl(src);
#ifdef TMET_ENABLED
    {
        unsigned long long curTime;
        const char searchStr[5] = "/dms";
        char *ret;

        ret = strstr(gCurrentPlaySrcUrl, searchStr);

        pthread_mutex_lock(&cgmiCliMutex);
        tMets_getMsSinceEpoch(&curTime);
        tMets_cacheMilestoneExt( TMETS_OPERATION_CHANELCHANGE,
                              gCurrentPlaySrcUrl,
                              curTime,
                              "CGMICLI_LOAD",
                              NULL,
                              ret);
        pthread_mutex_unlock(&cgmiCliMutex);
    }
#endif // TMET_ENABLED
    /* First load the URL. */
    retCode = cgmi_Load( pSessionId, src, cpblob, sessionSettings );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI Load failed: %s\n", cgmi_ErrorString(retCode) );
    }

    return retCode;
}


static cgmi_Status setstate_play(void *pSessionId, int autoPlay)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

    /* Play the URL if load succeeds. */
    gAutoPlay = autoPlay;
    retCode = cgmi_Play( pSessionId, autoPlay );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI Play failed: %s\n", cgmi_ErrorString(retCode) );
    }

    return retCode;
}

/* Play Command */
static cgmi_Status play(void *pSessionId, char *src, int autoPlay, cpBlobStruct * cpblob)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

    retCode = load(pSessionId, src, cpblob, NULL);
    if ( retCode != CGMI_ERROR_SUCCESS )
    {
        return retCode;
    }

    retCode = setstate_play(pSessionId, autoPlay);

    return retCode;
}

/* Resume Command */
static cgmi_Status resume(void *pSessionId, char *src, float resumePosition, int autoPlay, cpBlobStruct * cpblob)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

    /* First load the URL. */
    retCode = load(pSessionId, src, cpblob, NULL);
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI Load failed\n");
    } else {

        /* Set the position to resume position if it is positive. */
        if (resumePosition > 0)
        {
            retCode = cgmi_SetPosition( pSessionId, resumePosition );
            if (retCode != CGMI_ERROR_SUCCESS)
            {
                printf("CGMI cgmi_SetPosition Failed\n");
            }
        }

        if (retCode == CGMI_ERROR_SUCCESS)
        {
            /* Play the URL. */
            gAutoPlay = autoPlay;
            retCode = setstate_play(pSessionId, autoPlay);
            if (retCode != CGMI_ERROR_SUCCESS)
            {
                printf("CGMI Play failed\n");
            }
        }
    }

    return retCode;
}

/* Stop Command */
static cgmi_Status stop(void *pSessionId)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

#ifdef TMET_ENABLED
    {
        unsigned long long curTime;

        pthread_mutex_lock(&cgmiCliMutex);
        tMets_getMsSinceEpoch(&curTime);
        tMets_cacheMilestone( TMETS_OPERATION_CHANELCHANGE,
                              gCurrentPlaySrcUrl,
                              curTime,
                              "CGMICLI_STOP",
                              NULL);
        pthread_mutex_unlock(&cgmiCliMutex);
    }
#endif // TMET_ENABLED

    /* Destroy the section filter */
    retCode = destroyfilter(pSessionId);

    /* Stop = Unload */
    retCode = cgmi_Unload( pSessionId );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI Unload failed\n");
    }

    return retCode;
}

/* GetPosition Command */
static cgmi_Status getposition(void *pSessionId, float *pPosition)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

    retCode = cgmi_GetPosition( pSessionId, pPosition );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI GetPosition Failed\n");
    }

    return retCode;
}

/* SetPosition Command */
static cgmi_Status setposition(void *pSessionId, float Position)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

    retCode = cgmi_SetPosition( pSessionId, Position );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI SetPosition Failed\n");
    }

    return retCode;
}

/* GetRateRange Command */
static cgmi_Status getrates(void *pSessionId, float pRates[], unsigned int *pNumRates)
{
    cgmi_Status retCode = CGMI_ERROR_FAILED;

    retCode = cgmi_GetRates( pSessionId, pRates, pNumRates );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI GetRates Failed\n");
    }

    return retCode;
}

/* SetRate Command */
static cgmi_Status setrate(void *pSessionId, float Rate)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

    retCode = cgmi_SetRate( pSessionId, Rate );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI SetRate Failed\n");
    }

    return retCode;
}

/* GetDuration Command */
static cgmi_Status getduration(void *pSessionId, float *pDuration, cgmi_SessionType *type)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

    retCode = cgmi_GetDuration( pSessionId, pDuration, type );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI GetDuration Failed\n");
    }

    return retCode;
}

/****************************  Section Filter Callbacks  *******************************/
static cgmi_Status cgmi_QueryBufferCallback(
    void *pUserData,
    void *pFilterPriv,
    void *pFilterId,
    char **ppBuffer,
    int *pBufferSize )
{
    //g_print( "cgmi_QueryBufferCallback -- pFilterId: 0x%08lx \n", pFilterId );


    // Preconditions
    if( NULL == ppBuffer )
    {
        g_print("NULL buffer pointer passed to cgmiQueryBufferCallback.\n");
        return CGMI_ERROR_BAD_PARAM;
    }

    // Check if a size of greater than zero was provided, use default if not
    if( *pBufferSize <= 0 )
    {
        *pBufferSize = 256;
    }

    //g_print("Allocating buffer of size (%d)\n", *pBufferSize);
    *ppBuffer = g_malloc0(*pBufferSize);

    if( NULL == *ppBuffer )
    {
        *pBufferSize = 0;
        return CGMI_ERROR_OUT_OF_MEMORY;
    }

    return CGMI_ERROR_SUCCESS;
}

static cgmi_Status cgmi_SectionBufferCallback(
    void *pUserData,
    void *pFilterPriv,
    void *pFilterId,
    cgmi_Status sectionStatus,
    char *pSection,
    int sectionSize)
{
    cgmi_Status retStat;
    //tPat pat;
    int i = 0;

    //g_print( "cgmi_QueryBufferCallback -- pFilterId: 0x%08lx \n", pFilterId );

    if( NULL == pSection )
    {
        g_print("NULL buffer passed to cgmiSectionBufferCallback.\n");
        return CGMI_ERROR_BAD_PARAM;
    }

    g_print("Received section pFilterId: %p, sectionSize %d\n\n",
            pFilterId, sectionSize);
    for ( i=0; i < sectionSize; i++ )
    {
        printf( "0x%x ", (unsigned char) pSection[i] );
    }
    printf("\n");

    // After printing the PAT stop the filter
    g_print("Calling cgmi_StopSectionFilter...\n");
    retStat = cgmi_StopSectionFilter( pFilterPriv, pFilterId );
    if (retStat != CGMI_ERROR_SUCCESS )
    {
        printf("CGMI StopSectionFilterFailed\n");
    }

    // TODO:  Create a new filter to get a PMT found in the PAT above.
    filterRunning = false;
    // Free buffer allocated in cgmi_QueryBufferCallback
    g_free( pSection );

    return CGMI_ERROR_SUCCESS;
}

/* Section Filter */
static cgmi_Status sectionfilter( void *pSessionId, gint pid, guchar *value,
                                  guchar *mask, gint length )
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;

    tcgmi_FilterData filterdata;

    retCode = cgmi_CreateSectionFilter( pSessionId, pid, pSessionId, &filterid );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI CreateSectionFilter Failed\n");

        return retCode;
    }

    filterdata.value = value;
    filterdata.mask = mask;
    filterdata.length = length;
    filterdata.comparitor = FILTER_COMP_EQUAL;

    retCode = cgmi_SetSectionFilter( pSessionId, filterid, &filterdata );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI SetSectionFilter Failed\n");

        return retCode;
    }

    retCode = cgmi_StartSectionFilter( pSessionId, filterid, 10, 1, 0,
                                       cgmi_QueryBufferCallback, cgmi_SectionBufferCallback );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI StartSectionFilter Failed\n");
    }else
    {
        filterRunning = true;
    }

    return retCode;
}

static cgmi_Status destroyfilter( void *pSessionId )
{
    cgmi_Status retCode = CGMI_ERROR_FAILED ;

    if ( filterid != NULL )
    {
        if ( filterRunning == true )
        {
            retCode = cgmi_StopSectionFilter( pSessionId, filterid );
            if (retCode != CGMI_ERROR_SUCCESS )
            {
                printf("CGMI StopSectionFilterFailed\n");
            }
            filterRunning = false;
        }

        retCode = cgmi_DestroySectionFilter( pSessionId, filterid );
        if (retCode != CGMI_ERROR_SUCCESS )
        {
            printf("CGMI StopSectionFilterFailed\n");
        }

        filterid = NULL;
    }
    return retCode;
}

/* Callback Function */
static void cgmiCallback( void *pUserData, void *pSession, tcgmi_Event event, uint64_t code )
{
    switch (event)
    {
        case NOTIFY_STREAMING_OK:
            printf("NOTIFY_STREAMING_OK");
            break;
        case NOTIFY_FIRST_PTS_DECODED:
            printf("NOTIFY_FIRST_PTS_DECODED");
#ifdef TMET_ENABLED
            {
                unsigned long long curTime;

                pthread_mutex_lock(&cgmiCliMutex);
                tMets_getMsSinceEpoch(&curTime);
                tMets_cacheMilestone( TMETS_OPERATION_CHANELCHANGE,
                                      gCurrentPlaySrcUrl,
                                      curTime,
                                      "CGMICLI_FIRST_PTS_DECODED",
                                      "close");
                pthread_mutex_unlock(&cgmiCliMutex);

                tMets_postAllCachedMilestone(gDefaultPostUrl);
            }
#endif // TMET_ENABLED
            break;
        case NOTIFY_STREAMING_NOT_OK:
            printf("NOTIFY_STREAMING_NOT_OK");
            break;
        case NOTIFY_SEEK_DONE:
            printf("NOTIFY_SEEK_DONE");
            break;
        case NOTIFY_START_OF_STREAM:
            printf("NOTIFY_START_OF_STREAM");
            break;
        case NOTIFY_END_OF_STREAM:
            printf("NOTIFY_END_OF_STREAM");
            break;
        case NOTIFY_PSI_READY:
            {
               printf("NOTIFY_PSI_READY");
#ifdef TMET_ENABLED
               {
                unsigned long long curTime;

                pthread_mutex_lock(&cgmiCliMutex);
                tMets_getMsSinceEpoch(&curTime);
                tMets_cacheMilestone( TMETS_OPERATION_CHANELCHANGE,
                                      gCurrentPlaySrcUrl,
                                      curTime,
                                      "CGMICLI_ACQUIRED_PATPMT",
                                      NULL);
                pthread_mutex_unlock(&cgmiCliMutex);

                tMets_postAllCachedMilestone(gDefaultPostUrl);
               }
#endif // TMET_ENABLED
               gint i, count = 0;
               tcgmi_PidData pidData;
               cgmi_Status retCode;
               retCode = cgmi_GetNumPids( pSession, &count );
               if ( retCode != CGMI_ERROR_SUCCESS )
               {
                   printf("Error returned %d\n", retCode);
               }

               printf("\nAvailable streams: %d\n", count);
               printf("--------------------------\n");
               for ( i = 0; i < count; i++ )
               {
                   retCode = cgmi_GetPidInfo( pSession, i, &pidData );
                   if ( retCode != CGMI_ERROR_SUCCESS )
                       break;
                   printf("index = %d, pid = %d, stream type = %d\n", i, pidData.pid, pidData.streamType);
               }
               printf("--------------------------\n");
               if ( false == gAutoPlay )
               {
                  printf("Now set the A/V PID indices via the setpid command for decoding to start.\n");
                  printf("Decoding will start when the video PID index is set.\n");
                  printf("If the video PID does not exist, just set the index to anything.\n");
                  printf("Setting a PID index to -1 selects the first corresponding stream in the PMT.");
               }
#if 0
               usleep(3000000);
               printf("Setting pids...\n");
               cgmi_SetPidInfo( pSession, 1, 1, TRUE );
               cgmi_SetPidInfo( pSession, 0, 0, TRUE );
#endif
            }
            break;
        case NOTIFY_DECRYPTION_FAILED:
            printf("NOTIFY_DECRYPTION_FAILED");
            break;
        case NOTIFY_NO_DECRYPTION_KEY:
            printf("NOTIFY_NO_DECRYPTION_KEY");
            break;
        case NOTIFY_VIDEO_ASPECT_RATIO_CHANGED:
            printf("NOTIFY_VIDEO_ASPECT_RATIO_CHANGED");
            break;
        case NOTIFY_VIDEO_RESOLUTION_CHANGED:
            {
                int ar_denominator, ar_numerator, pixelHeight, pixelWidth;
                ar_denominator = code & 0xFFFF;
                code >>= 16;
                ar_numerator = code & 0xFFFF;
                code >>= 16;
                pixelHeight = code & 0xFFFF;
                code >>= 16;
                pixelWidth = code & 0xFFFF;
                /* tested in pytest, don't change output without modifying test expectations */
                printf("NOTIFY_VIDEO_RESOLUTION_CHANGED: %dx%d\n", pixelWidth, pixelHeight);
                printf("NOTIFY_ASPECT_RATIO_CHANGED: %d:%d", ar_numerator, ar_denominator);
            }
            break;
        case NOTIFY_CHANGED_LANGUAGE_AUDIO:
            printf("NOTIFY_CHANGED_LANGUAGE_AUDIO");
            break;
        case NOTIFY_CHANGED_LANGUAGE_SUBTITLE:
            printf("NOTIFY_CHANGED_LANGUAGE_SUBTITLE");
            break;
        case NOTIFY_CHANGED_LANGUAGE_TELETEXT:
            printf("NOTIFY_CHANGED_LANGUAGE_TELETEXT");
            break;
        case NOTIFY_MEDIAPLAYER_URL_OPEN_FAILURE:
            printf("NOTIFY_MEDIAPLAYER_URL_OPEN_FAILURE");
            break;
        case NOTIFY_CHANGED_RATE:
            printf("NOTIFY_CHANGED_RATE: %d", (gint) code);
            break;
        case NOTIFY_DECODE_ERROR:
            printf("NOTIFY_DECODE_ERROR");
            break;
        case NOTIFY_LOAD_DONE:
            {
               printf("NOTIFY_LOAD_DONE");
               cgmi_Status retCode;
               float Duration = 0.0;
               cgmi_SessionType type = cgmi_Session_Type_UNKNOWN;
               retCode = cgmi_GetDuration(pSession, &Duration, &type);
               if(CGMI_ERROR_SUCCESS != retCode)
               {
                  printf("\nGetDuration after URI load failed\n");
               }
               else
               {
                  printf("\nDuration after URI load = %f\n", Duration);
               }
            }
            break;
        case NOTIFY_NETWORK_ERROR:
            printf("NOTIFY_NETWORK_ERROR");
            break;
        case NOTIFY_MEDIAPLAYER_UNKNOWN:
            printf("NOTIFY_MEDIAPLAYER_UNKNOWN");
            break;

        default:
            printf("UNKNOWN");
            break;
    }

    printf( "\n" );
}

void help(void)
{
    printf( "Supported commands:\n"
            "Single APIs:\n"
           "\tload <url> Or load <url> <drmType> <cpBlob>\n"
           "\tsetstate_play [autoplay]\n"
           "\tplay <url> [autoplay]  Or play <url> <autoplay> <drmType> <cpBlob>\n"
           "\tresume <url> <position (seconds) (float)> [autoplay] Or resume  <url> <position (seconds) (float)> <autoplay> <drmType> <cpBlob>\n"
           "\tstop (or unload)\n"
           "\n"
           "\taudioplay <url>\n"
           "\taudiostop\n"
           "\n"
           "\tgetrates\n"
           "\tsetrate <rate (float)>\n"
           "\n"
           "\tgetposition\n"
           "\tsetposition <position (seconds) (float)>\n"
           "\n"
           "\tgetduration\n"
           "\n"
           "\tnewsession\n"
           "\n"
           "\tcreatefilter pid=0xVAL <value=0xVAL> <mask=0xVAL>\n"
           "\tstopfilter\n"
           "\n"
           "\tgetaudiolanginfo\n"
           "\tsetaudiolang <index>\n"
           "\tsetdefaudiolang <lang>\n"
          "\n"
          "\tgetsubtitleinfo\n"
          "\tsetdefsubtitlelang <lang>\n"
           "\n"
           "\tgetccinfo\n"
           "\n"
           "\tsetvideorect <srcx,srcy,srcw,srch,dstx,dsty,dstw,dsth>\n"
           "\n"
           "\tgetpidinfo\n"
           "\tsetpid <index> <A/V type (0:audio, 1:video)> <0:disable, 1:enable>\n"
           "\n"
           "\tsetlogging <GST_DEBUG format>\n"
           "\n"
           "\tgetvideodecoderindex\n"
           "\n"
           "\tgetvideores\n"
           "\n"
           "\tdumptimingentry\n"
            "\n"
           "\tdumpchannelchangetime\n"
            "\n"
           "\tresettimingentry\n"
           "\n"
           "\tgettsbslide\n"
           "\n"
           "\tgetpicturesetting <setting>\n"
           "\tsetpicturesetting <setting> <value>\n"
           "\t(Settings: CONTRAST, SATURATION, HUE, BRIGHTNESS, COLORTEMP, SHARPNESS)\n"
           "\t(-32768 <= value <= 32767)\n"
           "\n"
           "\tgetactivesessionsinfo\n"
           "\n"
           "Tests:\n"
           "\tcct <url #1> <url #2> <interval (seconds)> <duration(seconds)> [<1><drmType for url #1><cpBlob for url #1>] [<2><drmType for url #2><cpBlob for url #2>]\n"
           "\t\tChannel Change Test - Change channels between <url #1> and\n"
           "\t\t<url#2> at interval <interval> for duration <duration>.\n"
           "\n"
           "\tsessiontest <url> <interval (seconds)> <duration(seconds)> [<drmType> <cpBlob>]\n"
           "\t\tCreate and destroy session with playback of <url> in between\n"
           "\t\tat interval <interval> for duration <duration>.\n"
           "\n"
           "\n"
           "\thelp\n"
           "\thistory\n"
           "\n"
           "\tquit\n\n");
}

/* MAIN */
int main(int argc, char **argv)
{
    cgmi_Status retCode = CGMI_ERROR_SUCCESS;   /* Return codes from cgmi */
    void *pSessionId = NULL;                    /* CGMI Session Handle */
    void *pEasSessionId = NULL;                 /* CGMI Secondary (EAS) Audio Session Handle */

    gchar command[MAX_COMMAND_LENGTH];          /* Command buffer */
    gchar arg [MAX_COMMAND_LENGTH];              /* Command args buffer for
                                                   parsing. */
    gchar history[MAX_HISTORY][MAX_COMMAND_LENGTH]; /* History buffers */
    gint cur_history = 0;                       /* Current position in up/down
                                                   history browsing. */
    gint history_ptr = 0;                       /* Current location to store
                                                   new history. */
    gint history_depth = 0;                     /* How full is the history
                                                   buffer. */
    gint quit = 0;                              /* 1 = quit command parsing */
    gint playing = 0;                           /* Play state:
                                                    0: stopped
                                                    1: playing */

    gint audioplaying = 0;                      /* EAS Audio Play state:
                                                    0: stopped
                                                    1: playing */

    /* Command Parsing */
    int c = 0;      /* Character from console input. */
    int a = 0;      /* Character location in the command buffer. */
    int retcom = 0; /* Return command for processing. */

    /* Status Variables */
    gfloat Position = 0.0;
    gfloat Rate = 0.0;
    gfloat Rates[32];
    unsigned int NumRates = 32;
    gfloat Duration = 0.0;
    cgmi_SessionType type = cgmi_Session_Type_UNKNOWN;
    gint pbCanPlay = 0;

    /* Change Channel Test parameters */
    gchar url1[MAX_COMMAND_LENGTH], url2[MAX_COMMAND_LENGTH];
    gchar *str = NULL;
    gint interval = 0;
    gint duration = 0;
    struct timeval start, current;
    int i = 0, j = 0;



    /* createfilter parameters */
    gchar arg1[MAX_FILTER_LENGTH], arg2[MAX_FILTER_LENGTH], arg3[MAX_FILTER_LENGTH], arg4[MAX_FILTER_LENGTH], tmp[MAX_FILTER_LENGTH];
    gint pid = 0;
    guchar value[MAX_VALUE_LENGTH], mask[MAX_VALUE_LENGTH];
    gint vlength = 0, mlength = 0;
    gchar *ptmpchar;
    gchar tmpstr[3];
    int len = 0;
    int err = 0;
    cpBlobStruct cp_Blob_Struct, cp_Blob_Struct_2;

    memset(url1, 0, MAX_COMMAND_LENGTH);
    memset(url2, 0, MAX_COMMAND_LENGTH);
    memset(arg, 0, MAX_FILTER_LENGTH);
    memset(arg1, 0, MAX_FILTER_LENGTH);
    memset(arg2, 0, MAX_FILTER_LENGTH);
    memset(arg3, 0, MAX_FILTER_LENGTH);
    memset(arg4, 0, MAX_FILTER_LENGTH);
    memset(tmp, 0, MAX_FILTER_LENGTH);
    memset(value, 0, MAX_VALUE_LENGTH);
    memset(mask, 0, MAX_VALUE_LENGTH);



    memset(&cp_Blob_Struct, 0, sizeof(cp_Blob_Struct));
    memset(&cp_Blob_Struct_2, 0, sizeof(cp_Blob_Struct));
    cpBlobStruct * p_Cp_Blob_Struct = NULL;
    cpBlobStruct * p_Cp_Blob_Struct_2 = NULL;


    // need to put this in a define diagInit (DIAGTYPE_DEFAULT, NULL, 0);

#ifdef TMET_ENABLED
    /* Init timing metrics */
    if( tMets_Init() != TMET_STATUS_SUCCESS )
    {
       printf("tMets_Init() failed\n");
       return 1;
    }

    tMets_getDefaultUrl(&gDefaultPostUrl);
#endif // TMET_ENABLED

    unsigned long tsbSlide = 0;

    /* Init CGMI. */
    retCode = cgmi_Init();
    if(retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI Init Failed: %s\n", cgmi_ErrorString( retCode ));
        return 1;
    } else {
        printf("CGMI Init Success!\n");
    }

    /* Create a playback session. */
    retCode = cgmi_CreateSession( cgmiCallback, NULL, &pSessionId );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI CreateSession Failed: %s\n", cgmi_ErrorString( retCode ));
        return 1;
    } else {
        printf("CGMI CreateSession Success!\n");
    }

    /* Helpful Information */
    printf("CGMI CLI Ready...\n");
    help();

    /* Needed to handle key press events correctly. */
    tcgetattr( STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON); // Disable line buffering
    newt.c_lflag &= ~(ECHO); // Disable local echo
    tcsetattr( STDIN_FILENO, TCSANOW, &newt);

    /* Signal handler to clean up console. */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Main Command Loop */
    while (!quit)
    {
        retCode = CGMI_ERROR_SUCCESS;
        p_Cp_Blob_Struct = NULL;
        p_Cp_Blob_Struct_2 = NULL;
        /* commandline */
        printf( "cgmi> " );
        a = 0;
        retcom = 0;
        cur_history = history_ptr;
        while (!retcom)
        {
            c = getchar();
            switch (c)
            {
                case 0x1b:      /* Arrow keys */
                    c = getchar();
                    c = getchar();
                    if ( c == 0x41 )    /* Up */
                    {
                        cur_history--;
                        if ( cur_history < 0 )
                        {
                            if ( history_depth == MAX_HISTORY )
                            {
                                cur_history = MAX_HISTORY - 1;
                            } else {
                                cur_history = 0;
                            }
                        }

                        strncpy( command, history[cur_history],
                                 MAX_COMMAND_LENGTH );
                        a = strlen( command );
                        printf( "\ncgmi> %s", command );
                    }
                    if ( c == 0x42 )    /* Down */
                    {
                        cur_history++;
                        if ( cur_history >= MAX_HISTORY )
                        {
                            cur_history = MAX_HISTORY - 1;
                        }

                        if ( cur_history >= history_ptr )
                        {
                            cur_history = history_ptr;
                            command[0] = '\0';
                            a = 0;
                        } else {
                            strncpy( command, history[cur_history],
                                     MAX_COMMAND_LENGTH );
                            a = strlen( command );
                        }

                        printf( "\ncgmi> %s", command );
                    }
                    break;
                case 0x4:       /* Ctrl+D */
                    printf("\n");
                    quit = 1;
                    retcom = 1;
                    break;
                case 0x8:       /* Backspace */
                case 0x7f:
                    a--;
                    if (a<0)
                        a=0;
                    else
                        printf("\b \b");
                    break;
                case 0xa:       /* Enter */
                    printf( "\n" );
                    retcom = 1;
                    break;
                case 0x10:      /* Ctrl+P */
                    /* This is for fun.  Enjoy! */
                    printf( "\nYour pizza order has been placed and should"
                            " arrive in 20-30 minutes or it's FREE!!!\n" );
                    command[0] = '\0';
                    a = 0;
                    // cause a crash on purpose
                    crash ();
                    retcom = 1;
                    break;
                default:
                    /* Printable Characters */
                    if ( (c >= 0x20) && (c <= 0x7e) )
                    {
                        command[a++] = (char) c;
                        printf( "%c", c );
                    } else {
                        printf( "\nUnknown key: 0x%x\n", c );
                        printf( "\ncgmi> %s", command );
                    }
                    break;
            }
        }

        command[a] = '\0';
        if ( a > 0 )
        {
            //printf("command: %s\n", command);
            strncpy( history[history_ptr++], command, MAX_COMMAND_LENGTH );
            if ( history_ptr > (MAX_HISTORY - 1) )
            {
                history_ptr = 0;
            }
            if ( history_depth < MAX_HISTORY )
            {
                history_depth++;
            }
        }

        /* load */
        if (strncmp(command, "load", 4) == 0)
        {
            char *arg3;
            char *arg4;
            if ( strlen( command ) <= 5 )
            {
                printf( "\tload <url> Or load <url> <drmType> <cpBlob>\n" );
                continue;
            }
            strncpy( arg, command + 5, strlen(command) - 5 );
            arg[strlen(command) - 5] = '\0';

            arg3 = strchr( arg, ' ' );
            if (arg3) //drm_type_for cp blob(next param)
            {
                arg3++;
                memset(cp_Blob_Struct.drmType,0,MAX_DRM_TYPE_LENGTH);
                strncpy((char*)cp_Blob_Struct.drmType,arg3, MAX_DRM_TYPE_LENGTH -1 );
                arg4 = strchr( arg3, ' ' );
                if (arg4) //cp blob
                {
                    arg4++;
                    cp_Blob_Struct.bloblength=strlen(arg4);
                    memset(cp_Blob_Struct.cpBlob, 0, MAX_CP_BLOB_LENGTH);
                    memcpy(cp_Blob_Struct.cpBlob, arg4, cp_Blob_Struct.bloblength);
                    p_Cp_Blob_Struct = &cp_Blob_Struct;
                }
                else
                {
                    printf( "\tload <url> <drmType> <cpBlob>\n" );
                    continue;
                }
            }

            /* Check First */
            printf("Checking if we can play this...");
            retCode = cgmi_canPlayType( arg, &pbCanPlay );

            if ( retCode == CGMI_ERROR_NOT_IMPLEMENTED )
            {
                printf( "cgmi_canPlayType Not Implemented\n" );
                printf( "Playing \"%s\"...\n", arg );
                retCode = load(pSessionId, arg, p_Cp_Blob_Struct, NULL);
                if ( retCode == CGMI_ERROR_SUCCESS )
                {
                    playing = 1;
                }
            } else if ( !retCode && pbCanPlay )
            {
                printf( "Yes\n" );
                printf( "Playing \"%s\"...\n", arg );
                retCode = load(pSessionId, arg, p_Cp_Blob_Struct, NULL);
                if ( retCode == CGMI_ERROR_SUCCESS )
                {
                    playing = 1;
                }
            } else {
                printf( "No\n" );
            }
        }
        /* setstate_play */
        else if (strncmp(command, "setstate_play", 13) == 0)
        {
            int autoPlay = true;

            if ( strlen( command ) > 14 )
            {
                strncpy( arg, command + 14, strlen(command) - 14 );
                arg[strlen(command) - 14] = '\0';

                if ( arg )
                {
                   autoPlay = atoi( arg );
                }
            }

            setstate_play(pSessionId, autoPlay);
        }
        /* play */
        else if (strncmp(command, "play", 4) == 0)
        {
            char *arg2;
            char *arg3;
            char *arg4;
            int autoPlay = true;

            if ( strlen( command ) <= 5 )
            {
                printf( "\tplay <url> [autoplay]  Or play <url> <autoplay> <drmType> <cpBlob>\n" );
                continue;
            }
            strncpy( arg, command + 5, strlen(command) - 5 );
            arg[strlen(command) - 5] = '\0';

            arg2 = strchr( arg, ' ' );
            if ( arg2 )
            {
                *arg2 = 0;
                arg2++;
                autoPlay = atoi( arg2 );
                arg3 = strchr( arg2, ' ' );
                if (arg3) //drm_type_for cp blob(next param)
                {
                    arg3++;
                    memset(cp_Blob_Struct.drmType,0,MAX_DRM_TYPE_LENGTH);
                    strncpy((char*)cp_Blob_Struct.drmType,arg3, MAX_DRM_TYPE_LENGTH -1 );
                    arg4 = strchr( arg3, ' ' );
                    if (arg4) //cp blob
                    {
                        arg4++;
                        cp_Blob_Struct.bloblength=strlen(arg4);
                        memset(cp_Blob_Struct.cpBlob, 0, MAX_CP_BLOB_LENGTH);
                        memcpy(cp_Blob_Struct.cpBlob,arg4,cp_Blob_Struct.bloblength);
                        p_Cp_Blob_Struct=&cp_Blob_Struct;
                    }
                    else
                    {
                        printf( "\tplay <url> <autoplay> <drmType> <cpBlob>\n" );
                        continue;
                    }
        }
            }

            if (playing)
            {
                printf( "Stop previous playback before starting a new one.\n" );

                //update current playing url so stop can be count as part current channel change
                updateCurrentPlaySrcUrl(arg);

                retCode = stop(pSessionId);
                playing = 0;
            }

            /* Check First */
            printf("Checking if we can play this...");
            retCode = cgmi_canPlayType( arg, &pbCanPlay );

            if ( retCode == CGMI_ERROR_NOT_IMPLEMENTED )
            {
                printf( "cgmi_canPlayType Not Implemented\n" );
                printf( "Playing \"%s\"...\n", arg );
                retCode = play(pSessionId, arg, autoPlay, p_Cp_Blob_Struct);
                if ( retCode == CGMI_ERROR_SUCCESS )
                {
                    playing = 1;
                }
            } else if ( !retCode && pbCanPlay )
            {
                printf( "Yes\n" );
                printf( "Playing \"%s\"...\n", arg );
                retCode = play(pSessionId, arg, autoPlay, p_Cp_Blob_Struct);
                if ( retCode == CGMI_ERROR_SUCCESS )
                {
                    playing = 1;
                }
            } else {
                printf( "No\n" );
            }
        }
        /* resume */
        else if (strncmp(command, "resume", 6) == 0)
        {
            char *arg2;
            char  *arg3, *arg4, *arg5;
            int autoPlay = true;
            float resumePosition = 0.0;

            if (playing)
            {
                printf( "Stop previous playback before starting a new one.\n" );
                retCode = stop(pSessionId);
                playing = 0;
            }
            if ( strlen( command ) <= 7 )
            {
                printf( "\tresume <url> <position (seconds) (float)> [autoplay] Or resume  <url> <position (seconds) (float)> <autoplay> <drmType> <cpBlob>\n" );
                continue;
            }
            strncpy( arg, command + 7, strlen(command) - 7 );
            arg[strlen(command) - 7] = '\0';

            arg2 = strchr( arg, ' ' );
            if ( arg2 )
            {
               *arg2 = 0;
               arg2++;
               resumePosition = atof( arg2 );
            }
            else
            {
                printf( "\tresume <url> <position (seconds) (float)> [autoplay] Or resume  <url> <position (seconds) (float)> <autoplay> <drmType> <cpBlob>\n" );
                continue;
            }

            arg3 = strchr( arg2, ' ' );
            if ( arg3 )
            {
                *arg3 = 0;
                arg3++;
                autoPlay = atoi( arg3 );
                arg4 = strchr( arg3, ' ' );
                if (arg4) //drm_type_for cp blob(next param)
                {
                    arg4++;
                    memset(cp_Blob_Struct.drmType,0,MAX_DRM_TYPE_LENGTH);
                    strncpy((char*)cp_Blob_Struct.drmType,arg4, MAX_DRM_TYPE_LENGTH -1 );
                    arg5 = strchr( arg4, ' ' );
                    if (arg5) //cp blob
                    {
                        arg5++;
                        cp_Blob_Struct.bloblength=strlen(arg5);
                        memset(cp_Blob_Struct.cpBlob, 0, MAX_CP_BLOB_LENGTH);
                        memcpy(cp_Blob_Struct.cpBlob, arg5, cp_Blob_Struct.bloblength);
                        p_Cp_Blob_Struct = &cp_Blob_Struct;
                    }
                    else
                    {
                        printf( "\tresume  <url> <position (seconds) (float)> <autoplay> <drmType> <cpBlob>\n" );
                        continue;
                    }
        }
            }

            /* Check First */
            printf("Checking if we can play this...");
            retCode = cgmi_canPlayType( arg, &pbCanPlay );

            if ( retCode == CGMI_ERROR_NOT_IMPLEMENTED )
            {
                printf( "cgmi_canPlayType Not Implemented\n" );
                printf( "Resuming \"%s\"...\n", arg );
                retCode = resume(pSessionId, arg, resumePosition, autoPlay, p_Cp_Blob_Struct);
                if ( retCode == CGMI_ERROR_SUCCESS )
                {
                    playing = 1;
                }
            } else if ( !retCode && pbCanPlay )
            {
                printf( "Yes\n" );
                printf( "Resuming \"%s\"...\n", arg );
                retCode = resume(pSessionId, arg, resumePosition, autoPlay, p_Cp_Blob_Struct);
                if ( retCode == CGMI_ERROR_SUCCESS )
                {
                    playing = 1;
                }
            } else {
                printf( "No\n" );
            }
        }
        /* Stop Section Filter */
        else if (strncmp(command, "stopfilter", 10) == 0)
        {
            if ( filterid != NULL )
            {
                /* Stop/Destroy the section filter */
                retCode = destroyfilter(pSessionId);
                printf("Filter stopped.\n");
            } else {
                printf("Filter has not been started.\n");
            }
        }
        /* stop or unload */
        else if (
                ((strncmp(command, "stop", 4) == 0) && (strlen(command) == 4))
                || (strncmp(command, "unload", 6) == 0))
        {
            retCode = stop(pSessionId);
            playing = 0;
        }
        /* getrates */
        else if (strncmp(command, "getrates", 8) == 0)
        {
            /* Pass in the size of the array for NumRates... */
            retCode = getrates(pSessionId, Rates, &NumRates);
            /* Get back the actual number of elements filled in for NumRates. */
            if (!retCode)
            {
                printf( "Rates: " );
                for ( i=0; i<NumRates; i++ )
                {
                    printf( "%f ", Rates[i] );
                }
                printf( "\n" );
            }
            /* Be sure to reset the size of the array for the next call. */
            NumRates = 32;
        }
        /* setrate */
        else if (strncmp(command, "setrate", 7) == 0)
        {
            if ( strlen( command ) <= 8 )
            {
                printf( "\tsetrate <rate (float)>\n" );
                continue;
            }
            strncpy( arg, command + 8, strlen(command) - 8 );
            arg[strlen(command) - 8] = '\0';
            Rate = (float) atof(arg);
            printf( "Setting rate to %f (%s)\n", Rate, arg );
            retCode = setrate(pSessionId, Rate);
        }
        /* getposition */
        else if (strncmp(command, "getposition", 11) == 0)
        {
            retCode = getposition(pSessionId, &Position);
            if (!retCode)
            {
                printf( "Position: %f\n", Position );
            }
        }
        /* setposition */
        else if (strncmp(command, "setposition", 11) == 0)
        {
            if ( strlen( command ) <= 12 )
            {
                printf( "\tsetposition <position (seconds) (float)>\n" );
                continue;
            }
            strncpy( arg, command + 12, strlen(command) - 12 );
            arg[strlen(command) - 12] = '\0';
            Position = (float) atof(arg);
            printf( "Setting position to %f (%s)\n", Position, arg );
            retCode = setposition(pSessionId, Position);
        }
        /* getduration */
        else if (strncmp(command, "getduration", 11) == 0)
        {
            retCode = getduration(pSessionId, &Duration, &type);
            if (!retCode)
            {
                printf( "Duration: %f, SessionType: ", Duration );
                switch( type )
                {
                    case LIVE:
                        printf( "LIVE\n" );
                        break;
                    case FIXED:
                        printf( "FIXED\n" );
                        break;
                    case cgmi_Session_Type_UNKNOWN:
                        printf( "cgmi_Session_Type_UNKNOWN\n" );
                        break;
                    default:
                        printf( "ERROR\n" );
                        break;
                }
            }
        }
        /* New Session */
        else if (strncmp(command, "newsession", 10) == 0)
        {
            /* If we were playing, stop. */
            if ( playing )
            {
                printf("Stopping playback.\n");
                retCode = stop(pSessionId);
                playing = 0;
            }

            /* Destroy the created session. */
            if ( NULL != pSessionId )
            {
               retCode = cgmi_DestroySession( pSessionId );
               if (retCode != CGMI_ERROR_SUCCESS)
               {
                   printf("CGMI DestroySession Failed: %s\n",
                           cgmi_ErrorString( retCode ));
                   break;
               } else {
                   printf("CGMI DestroySession Success!\n");
                   pSessionId = NULL;
               }
            }

            /* Create a playback session. */
            retCode = cgmi_CreateSession( cgmiCallback, NULL, &pSessionId );
            if (retCode != CGMI_ERROR_SUCCESS)
            {
                printf("CGMI CreateSession Failed: %s\n", cgmi_ErrorString( retCode ));
                break;;
            } else {
                printf("CGMI CreateSession Success!\n");
            }
        }
        /* Create Filter */
        else if (strncmp(command, "createfilter", 12) == 0)
        {
            if ( filterid != NULL )
            {
                printf( "Only one filter at a time allowed currently in cgmi_cli.\n" );
                continue;
            }
            /* default values for section filter code */
            err = 0;
            pid = SECTION_FILTER_EMPTY_PID;
            bzero( value, 208 * sizeof( guchar ) );
            bzero( mask, 208 * sizeof( guchar ) );
            vlength = 0;
            mlength = 0;

            /* command */
            str = strtok( command, " " );
            if ( str == NULL )
            {
                printf( "Invalid command:\n"
                        "\tcreatefilter pid=0xVAL <value=0xVAL> <mask=0xVAL>\n"
                      );
                continue;
            }

            /* arg1 */
            str = strtok( NULL, " " );
            if ( str == NULL )
            {
                printf( "Invalid command:\n"
                        "\tcreatefilter pid=0xVAL <value=0xVAL> <mask=0xVAL>\n"
                      );
                continue;
            } else {
                strncpy( arg1, str, 256 );
            }

            /* arg2 */
            str = strtok( NULL, " " );
            if ( str != NULL )
            {
                strncpy( arg2, str, 256 );
            } else {
                strcpy( arg2, "" );
            }

            /* arg3 */
            str = strtok( NULL, " " );
            if ( str != NULL )
            {
                strncpy( arg3, str, 256 );
            } else {
                strcpy( arg3, "" );
            }

            /* arg4 */
            str = strtok( NULL, " " );
            if ( str != NULL )
            {
                strncpy( arg4, str, 256 );
            } else {
                strcpy( arg4, "" );
            }

            for ( i=0; i < 4; i++ )
            {
                /* cycle through each argument so they're supported out of order */
                switch (i)
                {
                    case 0:
                        strcpy( tmp, arg1 );
                        break;
                    case 1:
                        strcpy( tmp, arg2 );
                        break;
                    case 2:
                        strcpy( tmp, arg3 );
                        break;
                    case 3:
                        strcpy( tmp, arg4 );
                        break;
                }

                /* pid */
                if ( strncmp( tmp, "pid=", 4 ) == 0 )
                {
                    pid = (int) strtol( tmp + 4, NULL, 0 );
                }

                /* value */
                if ( strncmp( tmp, "value=", 6 ) == 0 )
                {
                    if ( strncmp( tmp + 6, "0x", 2 ) != 0 )
                    {
                        printf( "Hex values required for value.\n" );
                        err = 1;
                        break;
                    }
                    ptmpchar = tmp + 8;
                    vlength = 0;
                    len = strlen( ptmpchar );
                    while ( (ptmpchar[0] != '\0') && (ptmpchar[0] != '\n') && (len > (vlength * 2)) )
                    {
                        strncpy( tmpstr, ptmpchar, 2 );
                        tmpstr[2] = '\0';
                        value[vlength] = (guchar) strtoul( tmpstr, NULL, 16 );
                        ptmpchar = ptmpchar + 2;
                        vlength++;
                    }
                }

                /* mask */
                if ( strncmp( tmp, "mask=", 5 ) == 0 )
                {
                    if ( strncmp( tmp + 5, "0x", 2 ) != 0 )
                    {
                        printf( "Hex values required for mask.\n" );
                        err = 1;
                        break;
                    }
                    ptmpchar = tmp + 7;
                    mlength = 0;
                    len = strlen( ptmpchar );
                    while ( (ptmpchar[0] != '\0') && (ptmpchar[0] != '\n') && (len > (mlength * 2)) )
                    {
                        strncpy( tmpstr, ptmpchar, 2 );
                        tmpstr[2] = '\0';
                        mask[mlength] = (guchar) strtoul( tmpstr, NULL, 16 );
                        ptmpchar = ptmpchar + 2;
                        mlength++;
                    }
                }
            }

            if ( err )
            {
                continue;
            }

            if ( (mlength > 0) && (vlength > 0) && (mlength != vlength) )
            {
                printf( "Mask length and value length must be equal.\n" );

                continue;
            }

            retCode = sectionfilter( pSessionId, pid, value, mask, mlength );
            if ( retCode == CGMI_ERROR_SUCCESS )
            {
                printf( "Filter created.\n" );
            } else {
                printf( "Filter creation failed.\n" );
            }
        }
        /* get audio languages available */
        else if (strncmp(command, "getaudiolanginfo", 16) == 0)
        {
            gint count;
            gint i;
            gchar lang[4] = { 0 };
            retCode = cgmi_GetNumAudioLanguages( pSessionId, &count );
            if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
                continue;
            }
            printf("\nAvailable Audio Languages:\n");
            printf("--------------------------\n");
            for ( i = 0; i < count; i++ )
            {
                char isEnabled;
                retCode = cgmi_GetAudioLangInfo( pSessionId, i, lang, sizeof(lang), &isEnabled );
                if ( retCode != CGMI_ERROR_SUCCESS )
                    break;
                printf("%d: %s\n", i, lang);
            }
        }
        /* set audio stream */
        else if (strncmp(command, "setaudiolang", 12) == 0)
        {
            gint index;
            if ( strlen( command ) <= 13 )
            {
                printf( "\tsetaudiolang <index>\n" );
                continue;
            }
            strncpy( arg, command + 13, strlen(command) - 13 );
            arg[strlen(command) - 13] = '\0';

            index = atoi( arg );

            retCode = cgmi_SetAudioStream( pSessionId, index );
            if ( retCode == CGMI_ERROR_BAD_PARAM )
            {
                printf("Invalid index, use getaudiolanginfo to see available languages and their indexes %d\n", retCode);
            }
            else if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
            }
        }
        /* set default audio language */
        else if (strncmp(command, "setdefaudiolang", 15) == 0)
        {
            if ( strlen( command ) <= 16 )
            {
                printf( "\tsetdefaudiolang <lang>\n" );
                continue;
            }
            strncpy( arg, command + 16, strlen(command) - 16 );
            arg[strlen(command) - 16] = '\0';

            retCode = cgmi_SetDefaultAudioLang( pSessionId, arg );
            if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
            }
        }
        /* get closed caption services available */
        else if (strncmp(command, "getccinfo", 9) == 0)
        {
            gint count;
            gint i;
            gint serviceNum;
            gboolean isDigital;
            gchar lang[4] = { 0 };
            retCode = cgmi_GetNumClosedCaptionServices( pSessionId, &count );
            if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
                continue;
            }
            printf("\nAvailable Closed Caption Services:\n");
            printf("--------------------------\n");
            for ( i = 0; i < count; i++ )
            {
                retCode = cgmi_GetClosedCaptionServiceInfo( pSessionId, i, lang, sizeof(lang), &serviceNum, (char *)&isDigital );
                if ( retCode != CGMI_ERROR_SUCCESS )
                    break;
                printf("%d: %s, serviceNum: %d (%s)\n", i, lang, serviceNum, isDigital?"708":"608");
            }
        }
        /* set video rectangle */
        else if (strncmp(command, "setvideorect", 12) == 0)
        {
            char *ptr;
            char *dim;
            int i, size[8];
            if ( strlen( command ) <= 13 )
            {
                printf( "\tsetvideorect <srcx,srcy,srcw,srch,dstx,dsty,dstw,dsth>\n" );
                continue;
            }
            strncpy( arg, command + 13, strlen(command) - 13 );
            arg[strlen(command) - 13] = '\0';

            dim = arg;
            for ( i = 0; i < 8; i++ )
            {
                if ( i != 7 )
                    ptr = strchr(dim, ',');

                if ( NULL == ptr )
                {
                  printf("Error parsing arguments, please specify rectangle dimensions in the format srcx,srcy,srcw,srch,dstx,dsty,dstw,dsth\n");
                  break;
                }
                if ( i != 7 )
                    *ptr = 0;

                size[i] = atoi( dim );
                dim = ptr + 1;
            }

            if ( NULL == ptr )
                continue;

            retCode = cgmi_SetVideoRectangle( pSessionId, size[0], size[1], size[2], size[3], size[4], size[5], size[6], size[7] );
            if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
            }
        }
        /* get video source resolution */
        else if (strncmp(command, "getvideores", 11) == 0)
        {
            gint srcw, srch;

            retCode = cgmi_GetVideoResolution( pSessionId, &srcw, &srch );
            if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
                continue;
            }
            printf("\nVideo Source Resolution: %dx%d\n", srcw, srch);
        }
        /* get video decoder index */
        else if (strncmp(command, "getvideodecoderindex", 20) == 0)
        {
            gint ndx;

            retCode = cgmi_GetVideoDecoderIndex( pSessionId, &ndx );
            if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
                continue;
            }
            printf("\nVideo Decoder Index: %d\n", ndx);
        }
        /* get num pids */
        else if (strncmp(command, "getpidinfo", 10) == 0)
        {
            gint count = 0;
            tcgmi_PidData pidData;
            retCode = cgmi_GetNumPids( pSessionId, &count );
            if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
            }

            printf("\nAvailable streams: %d\n", count);
            printf("--------------------------\n");
            for ( i = 0; i < count; i++ )
            {
                retCode = cgmi_GetPidInfo( pSessionId, i, &pidData );
                if ( retCode != CGMI_ERROR_SUCCESS )
                    break;
                printf("%d: pid = %d, stream type = %d\n", i, pidData.pid, pidData.streamType);
            }
        }
        /* set pid */
        else if (strncmp(command, "setpid", 6) == 0)
        {
            gint index;
            gint type;
            gint enable;
            char *arg2, *arg3;
            if ( strlen( command ) <= 7 )
            {
                printf( "\tsetpid <index> <A/V type (0:audio, 1:video)> <0:disable, 1:enable>\n" );
                continue;
            }
            strncpy( arg, command + 7, strlen(command) - 7 );
            arg[strlen(command) - 13] = '\0';

            arg2 = strchr( arg, ' ' );
            if ( arg2 )
            {
               *arg2 = 0;
               arg2++;
            }
            else
            {
               printf( "\tsetpid <index> <A/V type (0:audio, 1:video)> <1:enable, 0:disable>\n" );
                continue;
            }

            arg3 = strchr( arg2, ' ' );
            if ( arg3 )
            {
               *arg3 = 0;
               arg3++;
            }
            else
            {
               printf( "\tsetpid <index> <A/V type (0:audio, 1:video)> <0:disable, 1:enable>\n" );
                continue;
            }


            index = atoi( arg );
            type = atoi( arg2 );
            enable = atoi( arg3 );

            retCode = cgmi_SetPidInfo( pSessionId, index, type, enable );
            if ( retCode == CGMI_ERROR_BAD_PARAM )
            {
                printf("Invalid index or type, use getpidinfo to see available indexes %d\n", retCode);
            }
            else if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
            }
        }
        /* play EAS audio */
        else if (strncmp(command, "audioplay", 9) == 0)
        {
            if ( strlen( command ) <= 10 )
            {
                printf( "\taudioplay <url>\n" );
                continue;
            }
            strncpy( arg, command + 10, strlen(command) - 10 );
            arg[strlen(command) - 10] = '\0';

            /* If we were playing EAS, stop. */
            if ( audioplaying )
            {
                printf("Stopping EAS playback.\n");
                retCode = stop( pEasSessionId );
                audioplaying = 0;
            }

            /* Destroy the EAS session. */
            if ( NULL != pEasSessionId )
            {
               retCode = cgmi_DestroySession( pEasSessionId );

               if (retCode != CGMI_ERROR_SUCCESS)
               {
                   printf("CGMI DestroySession Failed: %s\n",
                           cgmi_ErrorString( retCode ));
                   break;
               } else {
                   printf("CGMI DestroySession Success!\n");
                   pEasSessionId = NULL;
               }
            }

            /* Create a playback session. */
            retCode = cgmi_CreateSession( cgmiCallback, NULL, &pEasSessionId );

            if (retCode != CGMI_ERROR_SUCCESS)
            {
                printf("CGMI CreateSession Failed: %s\n", cgmi_ErrorString( retCode ));
                break;;
            } else {
                printf("CGMI CreateSession Success!\n");
            }

            /* Check First */
            printf("Checking if we can play this...");
            retCode = cgmi_canPlayType( arg, &pbCanPlay );
            if ( retCode == CGMI_ERROR_NOT_IMPLEMENTED )
            {
                printf( "cgmi_canPlayType Not Implemented\n" );
                printf( "Playing \"%s\"...\n", arg );
                cgmi_SetPidInfo( pSessionId, AUTO_SELECT_STREAM, STREAM_TYPE_AUDIO, FALSE );
                retCode = play(pEasSessionId, arg, TRUE, p_Cp_Blob_Struct);
                if ( retCode == CGMI_ERROR_SUCCESS )
                {
                    audioplaying = 1;
                }
            } else if ( !retCode && pbCanPlay )
            {
                printf( "Yes\n" );
                printf( "Playing \"%s\"...\n", arg );
                retCode = play(pEasSessionId, arg, TRUE, p_Cp_Blob_Struct);
                if ( retCode == CGMI_ERROR_SUCCESS )
                {
                    audioplaying = 1;
                }
            } else {
                printf( "No\n" );
            }
        }
        /* stop or unload */
        else if (strncmp(command, "audiostop", 9) == 0)
        {
            retCode = stop(pEasSessionId);
            cgmi_SetPidInfo( pSessionId, AUTO_SELECT_STREAM, STREAM_TYPE_AUDIO, TRUE );
            audioplaying = 0;
        }
        /* Channel Change Test */
        else if (strncmp(command, "cct", 3) == 0)
        {
            /* command */
            str = strtok( command, " " );
            if ( str == NULL ) continue;

            /* url1 */
            str = strtok( NULL, " " );
            if ( str == NULL ) continue;
            strncpy( url1, str, MAX_COMMAND_LENGTH );

            /* url2 */
            str = strtok( NULL, " " );
            if ( str == NULL ) continue;
            strncpy( url2, str, MAX_COMMAND_LENGTH );

            /* interval */
            str = strtok( NULL, " " );
            if ( str == NULL ) continue;
            interval = atoi( str );

            /* duration */
            str = strtok( NULL, " " );
            if ( str == NULL ) continue;
            duration = atoi( str );
            str = strtok( NULL, " " );
            if ( str != NULL )
            {
                if  (atoi( str ) == 1) //additional info for url 1
                {
                    str = strtok( NULL, " " );
                    if ( str == NULL ) continue;
                    memset(cp_Blob_Struct.drmType,0,MAX_DRM_TYPE_LENGTH);
                    strncpy((char*)cp_Blob_Struct.drmType,str, MAX_DRM_TYPE_LENGTH -1);
                    str = strtok( NULL, " " );
                    if ( str == NULL ) continue;
                    cp_Blob_Struct.bloblength = strlen(str);
                    memset(cp_Blob_Struct.cpBlob, 0, MAX_CP_BLOB_LENGTH);
                    memcpy(cp_Blob_Struct.cpBlob, str, cp_Blob_Struct.bloblength);
                    p_Cp_Blob_Struct = &cp_Blob_Struct;
                    str = strtok( NULL, " " );
                    if ( str != NULL )
                    {
                        if  (atoi( str ) == 2) //additional info for url 2
                        {
                            str = strtok( NULL, " " );
                            if ( str == NULL ) continue;
                            memset(cp_Blob_Struct.drmType,0,MAX_DRM_TYPE_LENGTH);
                            strncpy((char*)cp_Blob_Struct_2.drmType , str,MAX_DRM_TYPE_LENGTH -1 );
                            str = strtok( NULL, " " );
                            if ( str == NULL ) continue;
                            cp_Blob_Struct_2.bloblength = strlen(str);
                            memset(cp_Blob_Struct_2.cpBlob, 0, MAX_CP_BLOB_LENGTH);
                            memcpy(cp_Blob_Struct_2.cpBlob, str, cp_Blob_Struct_2.bloblength);
                            p_Cp_Blob_Struct_2 = &cp_Blob_Struct_2;
                        }
                        else
                        {
                            continue;
                        }
                    }
                }
                else if (atoi( str )==2) //additional info for url 2
                {
                    str = strtok( NULL, " " );
                    if ( str == NULL ) continue;
                    memset(cp_Blob_Struct.drmType,0,MAX_DRM_TYPE_LENGTH);
                    strncpy((char*)cp_Blob_Struct_2.drmType , str,MAX_DRM_TYPE_LENGTH -1 );
                    str = strtok( NULL, " " );
                    if ( str == NULL ) continue;
                    cp_Blob_Struct_2.bloblength=strlen(str);
                    memset(cp_Blob_Struct_2.cpBlob, 0, MAX_CP_BLOB_LENGTH);
                    memcpy(cp_Blob_Struct_2.cpBlob, str, cp_Blob_Struct_2.bloblength);
                    p_Cp_Blob_Struct_2 = &cp_Blob_Struct_2;
                }
                else //not legit
                {
                    continue;
                }
            }
            retCode = cgmi_canPlayType( url1, &pbCanPlay );
            if ( retCode == CGMI_ERROR_NOT_IMPLEMENTED )
            {
                printf( "cgmi_canPlayType Not Implemented\n" );
            } else if ( retCode || !pbCanPlay )
            {
                printf( "Cannot play %s\n", url1 );
                continue;
            }

            retCode = cgmi_canPlayType( url2, &pbCanPlay );
            if ( retCode == CGMI_ERROR_NOT_IMPLEMENTED )
            {
                printf( "cgmi_canPlayType Not Implemented\n" );
            } else if ( retCode || !pbCanPlay )
            {
                printf( "Cannot play %s\n", url2 );
                continue;
            }

            /* Run test. */
            gettimeofday( &start, NULL );
            gettimeofday( &current, NULL );
            str = url1;
            cpBlobStruct *p_Cp_Blob_Struct_Current = p_Cp_Blob_Struct;
            i = 0;
            while ( (current.tv_sec - start.tv_sec) < duration )
            {
                i++;
                printf( "(%d) Playing %s...\n", i, str );
                retCode = play(pSessionId, str, true, p_Cp_Blob_Struct_Current);
                sleep( interval );
                retCode = stop(pSessionId);
                if (pSessionId != NULL)
                {
                    retCode = cgmi_DestroySession( pSessionId );
                    if (retCode != CGMI_ERROR_SUCCESS)
                    {
                        printf("CGMI DestroySession Failed: %s\n", cgmi_ErrorString( retCode ));
                        gettimeofday( &current, NULL );
                        break;
                    }
                    else
                    {
                        printf("CGMI DestroySession Success!\n");
                        pSessionId = NULL;
                    }
                }

                /* Create a playback session. */
                retCode = cgmi_CreateSession( cgmiCallback, NULL, &pSessionId );
                if (retCode != CGMI_ERROR_SUCCESS)
                {
                    printf("CGMI CreateSession Failed: %s\n", cgmi_ErrorString( retCode ));
                    gettimeofday( &current, NULL );
                    break;;
                }
                else
                {
                    printf("CGMI CreateSession Success!\n");
                }

                if ( str == url1 )
                {
                    str = url2;
                    p_Cp_Blob_Struct_Current = p_Cp_Blob_Struct_2;
                }
                else
                {
                    str = url1;
                    p_Cp_Blob_Struct_Current = p_Cp_Blob_Struct;
                }

                gettimeofday( &current, NULL );
            }

            printf( "Played for %d seconds. %d channels.\n",
                    (int) (current.tv_sec - start.tv_sec), i );
        }
        /* Session Test */
        else if (strncmp(command, "sessiontest", 11) == 0)
        {
            /* command */
            str = strtok( command, " " );
            if ( str == NULL ) continue;

            /* url */
            str = strtok( NULL, " " );
            if ( str == NULL ) continue;
            strncpy( url1, str, MAX_COMMAND_LENGTH );

            /* interval */
            str = strtok( NULL, " " );
            if ( str == NULL ) continue;
            interval = atoi( str );

            /* duration */
            str = strtok( NULL, " " );
            if ( str == NULL ) continue;
            duration = atoi( str );
            str = strtok( NULL, " " );
            if ( str != NULL )
            {
                memset(cp_Blob_Struct.drmType,0,MAX_DRM_TYPE_LENGTH);
                strncpy((char*)cp_Blob_Struct.drmType , str,MAX_DRM_TYPE_LENGTH -1 );
                str = strtok( NULL, " " );
                if ( str == NULL ) continue;
                cp_Blob_Struct.bloblength = strlen(str);
                memset(cp_Blob_Struct.cpBlob, 0, MAX_CP_BLOB_LENGTH);
                memcpy(cp_Blob_Struct.cpBlob, str, cp_Blob_Struct.bloblength);
                p_Cp_Blob_Struct = &cp_Blob_Struct;
            }
            retCode = cgmi_canPlayType( url1, &pbCanPlay );
            if ( retCode == CGMI_ERROR_NOT_IMPLEMENTED )
            {
                printf( "cgmi_canPlayType Not Implemented\n" );
            } else if ( retCode || !pbCanPlay )
            {
                printf( "Cannot play %s\n", url1 );
                continue;
            }

            /* Run test. */
            gettimeofday( &start, NULL );
            gettimeofday( &current, NULL );
            i = 0;
            while ( (current.tv_sec - start.tv_sec) < duration )
            {
                i++;
                printf( "(%d) Playing %s...\n", i, url1 );
                retCode = play(pSessionId, url1, true, p_Cp_Blob_Struct);
                sleep( interval );
                retCode = stop(pSessionId);

                /* Destroy the created session. */
                if ( NULL != pSessionId )
                {
                   retCode = cgmi_DestroySession( pSessionId );
                   if (retCode != CGMI_ERROR_SUCCESS)
                   {
                       printf("CGMI DestroySession Failed: %s\n",
                               cgmi_ErrorString( retCode ));
                       break;
                   } else {
                       printf("CGMI DestroySession Success!\n");
                       pSessionId = NULL;
                   }
                }

                /* Create a playback session. */
                retCode = cgmi_CreateSession( cgmiCallback, NULL, &pSessionId );
                if (retCode != CGMI_ERROR_SUCCESS)
                {
                    printf("CGMI CreateSession Failed: %s\n", cgmi_ErrorString( retCode ));
                    break;;
                } else {
                    printf("CGMI CreateSession Success!\n");
                }

                gettimeofday( &current, NULL );
            }

            printf( "Played for %d seconds. %d channels.\n",
                    (int) (current.tv_sec - start.tv_sec), i );
        }

        /* set logging  */
        else if (strncmp(command, "setlogging", 10) == 0)
        {
            if ( strlen( command ) <= 11 )
            {
                printf( "\tsetlogging <GST_DEBUG format>\n" );
                continue;
            }
            strncpy( arg, command + 11, strlen(command) - 11 );
            arg[strlen(command) - 11] = '\0';

            retCode = cgmi_SetLogging(arg);
            if (retCode != CGMI_ERROR_SUCCESS)
            {
                printf("Error in set logging. Returned %d\n", retCode);
            }
        }

        /* dumptimingentry */
        else if (strncmp(command, "dumptimingentry", 15) == 0)
        {
            dumpTimingEntry();
            retCode = CGMI_ERROR_SUCCESS;
        }

        /* dumpchannelchangetime */
        else if (strncmp(command, "dumpchannelchangetime", 21) == 0)
        {
            dumpChannelChangeTime();
            retCode = CGMI_ERROR_SUCCESS;
        }

        /* resettimingentry */
        else if (strncmp(command, "resettimingentry", 16) == 0)
        {
            cgmiDiags_ResetTimingMetrics();
            retCode = CGMI_ERROR_SUCCESS;
        }

        /* help */
        else if (strncmp(command, "help", 4) == 0)
        {
            help();
        }
        /* history */
        else if (strncmp(command, "history", 7) == 0)
        {
            for ( j=0; j < history_depth; j++ )
            {
                printf( "\t%s\n", history[j] );
            }
        }
        /* quit */
        else if (strncmp(command, "quit", 4) == 0)
        {
            quit = 1;
        }
        else if (strncmp(command, "gettsbslide", 20) == 0)
        {
            retCode = cgmi_GetTsbSlide(pSessionId, &tsbSlide);
            if (CGMI_ERROR_SUCCESS == retCode)
            {
                printf( "TSBSlide: %lu\n", tsbSlide );
            }
            else
            {
               printf("CGMI GetTsbSlide Failed\n");
            }
        }
        /* get picture settings */
        else if (strncmp(command, "getpicturesetting", 17) == 0)
        {
            tcgmi_PictureCtrl pctl = -1;
            gint value = 0;
            if ( strlen( command ) <= 18 )
            {
                printf( "\tgetpicturesetting <setting>\n" );
                continue;
            }
            strncpy( arg, command + 18, strlen(command) - 18 );
            arg[strlen(command) - 18] = '\0';

            if ( !strncasecmp( arg, "CONTRAST", 8 ) )
            {
                pctl = PICTURE_CTRL_CONTRAST;
            }
            else if ( !strncasecmp( arg, "SATURATION", 10 ) )
            {
                pctl = PICTURE_CTRL_SATURATION;
            }
            else if ( !strncasecmp( arg, "HUE", 3 ) )
            {
                pctl = PICTURE_CTRL_HUE;
            }
            else if ( !strncasecmp( arg, "BRIGHTNESS", 10 ) )
            {
                pctl = PICTURE_CTRL_BRIGHTNESS;
            }
            else if ( !strncasecmp( arg, "COLORTEMP", 9 ) )
            {
                pctl = PICTURE_CTRL_COLORTEMP;
            }
            else if ( !strncasecmp( arg, "SHARPNESS", 9 ) )
            {
                pctl = PICTURE_CTRL_SHARPNESS;
            }

            retCode = cgmi_GetPictureSetting( pSessionId, pctl, &value );
            if ( retCode == CGMI_ERROR_BAD_PARAM )
            {
                printf("Invalid control %d\n", retCode);
            }
            else if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
            }
            else
            {
                printf("%s: %d\n", arg, value);
            }
        }

        /* set picture settings */
        else if (strncmp(command, "setpicturesetting", 17) == 0)
        {
            tcgmi_PictureCtrl pctl = -1;
            gint value;
            if ( strlen( command ) <= 18 )
            {
                printf( "\tsetpicturesetting <setting> <value>\n" );
                continue;
            }

            /* command */
            str = strtok( command, " " );
            if ( str == NULL )
            {
                printf( "Invalid command:\n"
                        "\tsetpicturesetting <setting> <value>n"
                      );
                continue;
            }

            /* arg1 */
            str = strtok( NULL, " " );
            if ( str == NULL )
            {
                printf( "Invalid command:\n"
                        "\tsetpicturesetting <setting> <value>\n"
                      );
                continue;
            } else {
                strncpy( arg1, str, 256 );
            }

            /* arg2 */
            str = strtok( NULL, " " );
            if ( str == NULL )
            {
                printf( "Invalid command:\n"
                        "\tsetpicturesetting <setting> <value>\n"
                      );
                continue;
            } else {
                strncpy( arg2, str, 256 );
            }

            if ( !strncasecmp( arg1, "CONTRAST", 8 ) )
            {
                pctl = PICTURE_CTRL_CONTRAST;
            }
            else if ( !strncasecmp( arg1, "SATURATION", 10 ) )
            {
                pctl = PICTURE_CTRL_SATURATION;
            }
            else if ( !strncasecmp( arg1, "HUE", 3 ) )
            {
                pctl = PICTURE_CTRL_HUE;
            }
            else if ( !strncasecmp( arg1, "BRIGHTNESS", 10 ) )
            {
                pctl = PICTURE_CTRL_BRIGHTNESS;
            }
            else if ( !strncasecmp( arg1, "COLORTEMP", 9 ) )
            {
                pctl = PICTURE_CTRL_COLORTEMP;
            }
            else if ( !strncasecmp( arg1, "SHARPNESS", 9 ) )
            {
                pctl = PICTURE_CTRL_SHARPNESS;
            }

            value = atoi( arg2 );

            retCode = cgmi_SetPictureSetting( pSessionId, pctl, value );
            if ( retCode == CGMI_ERROR_BAD_PARAM )
            {
                printf("Invalid control or value %d\n", retCode);
            }
            else if ( retCode != CGMI_ERROR_SUCCESS )
            {
                printf("Error returned %d\n", retCode);
            }
        }
      /* get subtitle languages available */
      else if ( strncmp(command, "getsubtitleinfo", 19) == 0 )
      {
         gint count;
         gint i;
         gushort compPageId, ancPageId, pid;
         guchar type;

         gchar lang[4] = { 0 };
         retCode = cgmi_GetNumSubtitleLanguages(pSessionId, &count);
         if ( retCode != CGMI_ERROR_SUCCESS )
         {
            printf("Error returned %d\n", retCode);
            continue;
         }
         printf("\nAvailable Subtitle Languages:\n");
         printf("--------------------------\n");
         for ( i = 0; i < count; i++ )
         {
            retCode = cgmi_GetSubtitleInfo(pSessionId, i, lang, sizeof(lang), &pid, &type, &compPageId, &ancPageId);
            if ( retCode != CGMI_ERROR_SUCCESS ) break;
            printf("%d: %s pid (%04x) type (%02x) compPageId (%04x) ancPageId (%04x)\n", i, lang, pid, type, compPageId, ancPageId);
         }
      }
      /* set default subtitle language */
      else if ( strncmp(command, "setdefsubtitlelang", 18) == 0 )
      {
         if ( strlen(command) <= 19 )
         {
            printf("\tsetdefsubtitlelang <lang>\n");
            continue;
         }
         strncpy(arg, command + 19, strlen(command) - 19);
         arg[strlen(command) - 19] = '\0';

         retCode = cgmi_SetDefaultSubtitleLang(pSessionId, arg);
         if ( retCode != CGMI_ERROR_SUCCESS )
         {
            printf("Error returned %d\n", retCode);
         }
      }
        else if ( strncmp(command, "getactivesessionsinfo", strlen("getactivesessionsinfo")) == 0 )
        {
           sessionInfo *sessInfoArr = NULL;
           int numSess = 0;
           int ii = 0;
           retCode = cgmi_GetActiveSessionsInfo(&sessInfoArr, &numSess);
           if ( retCode != CGMI_ERROR_SUCCESS )
           {
              printf("Error returned %d\n", retCode);
           }
           else
           {
              printf("Total active CGMI sessions: %d\n", numSess);
              if(NULL != sessInfoArr)
              {
                 for(ii = 0; ii < numSess; ii++)
                 {
                    printf("uri:%s, hwVideoDecoderHandle: %llu, hwAudioDecoderHandle: %llu\n",
                          sessInfoArr[ii].uri, sessInfoArr[ii].hwVideoDecHandle, sessInfoArr[ii].hwAudioDecHandle);

                 }
                 free(sessInfoArr);
                 sessInfoArr = NULL;
              }
           }
        }
        /* unknown */
        else
        {
            if ( command[0] != '\0' )
            {
                printf( "Unknown command: \"%s\"\n", command );
            }
        }

        /* If we receive and error, print error. */
        if (retCode)
        {
             printf( "Error: %s\n", cgmi_ErrorString( retCode ) );
        }
    }

    tcsetattr( STDIN_FILENO, TCSANOW, &oldt);

    /* If we were playing, stop. */
    if ( playing )
    {
        printf("Stopping playback.\n");
        retCode = stop(pSessionId);
    }

    /* Destroy the created session. */
    retCode = cgmi_DestroySession( pSessionId );
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI DestroySession Failed: %s\n", cgmi_ErrorString( retCode ));
    } else {
        printf("CGMI DestroySession Success!\n");
        pSessionId = NULL;
    }

    /* Terminate CGMI interface. */
    retCode = cgmi_Term();
    if (retCode != CGMI_ERROR_SUCCESS)
    {
        printf("CGMI Term Failed: %s\n", cgmi_ErrorString( retCode ));
    } else {
        printf("CGMI Term Success!\n");
    }

#ifdef TMET_ENABLED
    tMets_Term();
#endif // TMET_ENABLED

    return 0;
}
