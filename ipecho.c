/* -------------------------------------------------------------------------- *
 * ipecho.c                                                                   *
 *                                                                            *
 *  This program is free software; you can redistribute it and/or modify      *
 *  it under the terms of the GNU General Public License as published by      *
 *  the Free Software Foundation; either version 2 of the License, or         *
 *  (at your option) any later version.                                       *
 *                                                                            *
 *  This program is distributed in the hope that it will be useful,           *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
 *  GNU General Public License for more details.                              *
 *                                                                            *
 *  You should have received a copy of the GNU General Public License         *
 *  along with this program; if not, write to the Free Software               *
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA *
 * -------------------------------------------------------------------------- */
#define INCL_DOSNLS
#define INCL_DOSERRORS
#define INCL_VIO
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Sockets & networking
#include <types.h>
#include <sys\socket.h>
#include <sys\ioctl.h>
#include <net\route.h>
#include <net\if.h>
#include <net\if_arp.h>
#include <sys\time.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet\in.h>
#include <arpa\inet.h>

// -----------------------------------------------------------------------------
// Constants

#define MESSAGE_NORMAL              1       // normal output
#define MESSAGE_VERBOSE             2       // verbose output
#define MESSAGE_DIAGNOSTIC          3       // very verbose (diagnostic) output

#define UL_SELECT_TIMEOUT           5000    // timeout for os2_select() in ms
#define US_MAX_READS                3       // max. number of socket read attempts
#define US_MAX_SERVERS              3       // max. number of servers to try

#define UL_DATA_LIMIT               4096    // max. amount of data to read from socket

#define US_SZMESSAGE_MAX            1024    // various string length limits
#define US_SZCOMMAND_MAX            256
#define US_SZRESPONSE_MAX           512
#define US_SZCFG_MAX                1024
#define US_SZHOSTNAME_MAX           256
#define US_SZPATH_MAX               256
#define US_SZAFTER_MAX              512

// Various defaults (most of these aren't really used by the program)
#define US_DEFAULT_ECHO_PORT        80

#define SZ_DEFAULT_ECHO_SERVER      "checkip.dyndns.org"
#define SZ_DEFAULT_ECHO_DIR         "/"
#define SZ_DEFAULT_FIND_AFTER       "Address:"

#define SZ_CONFIG_FILE              "ipecho.cfg"  // name of our configuration file


// -----------------------------------------------------------------------------
// Typedefs

typedef struct sockaddr_in SOCKADDR_IN, *PSOCKADDR_IN;
typedef struct hostent     HOSTENT,     *PHOSTENT;
typedef struct timeval     TIMEVAL,     *PTIMEVAL;

typedef struct _Echo_Parms {
    ULONG  ulSocket;
    USHORT usPort;
    CHAR   szServer[ US_SZHOSTNAME_MAX + 1 ],
           szDir[ US_SZPATH_MAX + 1 ],
           szFindAfter[ US_SZAFTER_MAX + 1 ];
} ECHOPARMS, *PECHOPARMS;


// -----------------------------------------------------------------------------
// Function prototypes

SHORT ReadConfig( ECHOPARMS echo_p[], USHORT usVerbosity );
int   SocketConnect( char *pszDestIP, unsigned short usDestPort, unsigned short usVerbosity );
short SocketRead( int ulSocket, char *pszResponse );
short SocketWrite( int ulSocket, char *pszData );
ULONG EchoQuery( ECHOPARMS echo_p, USHORT usVerbosity );
CHAR  BoxChar( CHAR chPref, CHAR chPlain );
char *strstrip( char *s );
void  message_out( PSZ pszText, USHORT usLevel, USHORT usVerbosity );


/* -------------------------------------------------------------------------- *
 * -------------------------------------------------------------------------- */
int main( int argc, char *argv[] )
{
    ECHOPARMS echo_p[ US_MAX_SERVERS ]; // array of configured servers
    USHORT    usConfigured,             // number of configured servers
              usTry,                    // the current server being attempted
              usVerbosity;              // verbosity level
    ULONG     ulRC,
              a;

/*
 * Arguments:
 *      /v      Verbose output
 *      /vv     Very Verbose (diagnostic) output
 *      /?      Show help
 *
 */

    usVerbosity = MESSAGE_NORMAL;
    if ( argc > 1 ) {
        for ( a = 1; a < argc; a++ ) {
            if (( strnicmp( argv[a], "/?", 2 ) == 0 ) || ( strnicmp( argv[a], "/h", 2 ) == 0 )) {
                printf("IPEcho version 1.0. (C) 2006, 2018 Alexander Taylor.\n");
                printf("Licensed under version 2 of the GNU General Public License.\n\n");
                printf("Query the system's public IP address (as seen by an outside server).\n");
                printf("\nArguments:\n");
                printf("  /?   Show this help.\n");
                printf("  /V   Print detailed (verbose) output.\n");
                printf("  /VV  Print even more detailed (diagnostic) output.\n");
                return ( 0 );
            }
            else if ( strnicmp( argv[a], "/vv", 3 ) == 0 )
                usVerbosity = MESSAGE_DIAGNOSTIC;
            else if ( strnicmp( argv[a], "/v", 2 ) == 0 )
                    usVerbosity = MESSAGE_VERBOSE;
        }
    }

    // Get the configuration
    usConfigured = ReadConfig( echo_p, usVerbosity );
    if ( usConfigured < 1 ) {
        printf("No valid servers were found.\n");
        return ( 1 );
    }

    // Now try each server until we find one that works
    for ( usTry = 0; usTry < usConfigured; usTry++ ) {
        echo_p[ usTry ].ulSocket = SocketConnect( echo_p[ usTry ].szServer,
                                                  echo_p[ usTry ].usPort,
                                                  usVerbosity               );
        if ( echo_p[ usTry ].ulSocket ) {
            ulRC = EchoQuery( echo_p[ usTry ], usVerbosity );
            soclose( echo_p[ usTry ].ulSocket );
            if ( ulRC == NO_ERROR ) break;
        }
    }

    return ( 0 );
}


/* -------------------------------------------------------------------------- *
 * ReadConfig                                                                 *
 *                                                                            *
 * Parses the configuration file and generates the array of servers to try.   *
 * -------------------------------------------------------------------------- */
SHORT ReadConfig( ECHOPARMS echo_p[], USHORT usVerbosity )
{
    PSZ    pszEtc,                              // value of %ETC%
           token;                               // current token from strtok
    CHAR   szMessage[ US_SZMESSAGE_MAX + 1 ],   // output message
           szConfigFile[ CCHMAXPATH + 1 ],      // configuration file name
           szBuf[ US_SZCFG_MAX + 1 ],           // a configuration file entry
           szServer[ US_SZHOSTNAME_MAX + 1 ],   // hostname of server
           szDir[ US_SZPATH_MAX + 1 ],          // path on server
           szFindAfter[ US_SZAFTER_MAX + 1 ];   // "find after" text on server
    USHORT usPort,                              // IP port on server
           usCount;                             // number of servers configured
    FILE   *pfConfig;                           // configuration file handle


    // Look for a configuration file under %ETC%
    if (( pszEtc = getenv("ETC")) == NULL ) {
        sprintf( szMessage, "Unable to resolve ETC environment variable.\n");
        message_out( szMessage, MESSAGE_NORMAL, usVerbosity );
        return ( 0 );
    }
    sprintf( szConfigFile, "%s\\%s", pszEtc, SZ_CONFIG_FILE );
    strupr( szConfigFile );
    if (( pfConfig = fopen( szConfigFile, "r")) == NULL ) {
        sprintf( szMessage, "Configuration file %s could not be opened.\n", szConfigFile );
        message_out( szMessage, MESSAGE_NORMAL, usVerbosity );
        return ( 0 );
    }

    sprintf( szMessage, "Configuration file: %s\n", szConfigFile );
    message_out( szMessage, MESSAGE_DIAGNOSTIC, usVerbosity );

    // Read configuration entries from the file (one per line)
    usCount = 0;
    while ( !feof(pfConfig) ) {

        memset( szServer,    0, sizeof(szServer)    );
        memset( szDir,       0, sizeof(szDir)       );
        memset( szFindAfter, 0, sizeof(szFindAfter) );

        if ( fgets( szBuf, US_SZCFG_MAX, pfConfig ) == NULL ) break;
        strstrip( szBuf );

        // skip blank or commented lines
        if (( strlen(szBuf) == 0 ) || ( szBuf[0] == '#') || ( szBuf[0] == ';')) continue;

//        sprintf( szMessage, " > %s\n", szBuf );
//        message_out( szMessage, MESSAGE_DIAGNOSTIC, usVerbosity );

        // parse entry in the format: <host> <port> <path> <after>

        // hostname
        if (( token = strtok( szBuf, " ")) == NULL ) continue;
        strncpy( szServer, token, US_SZHOSTNAME_MAX );

//printf("server %s;", szServer );

        // port
        if (( token = strtok( NULL, " ")) == NULL ) continue;
        if ( sscanf( token, "%d", &usPort ) == 0 ) continue;

//printf("port %d;", usPort);

        // path
        if (( token = strtok( NULL, " ")) == NULL ) continue;
        strncpy( szDir, token, US_SZPATH_MAX );

//printf("port ");

        // after
        if (( token = strtok( NULL, "\0")) != NULL )
            strncpy( szFindAfter, token, US_SZAFTER_MAX );

        echo_p[usCount].usPort = usPort;
        strcpy( echo_p[usCount].szServer,    szServer    );
        strcpy( echo_p[usCount].szDir,       szDir       );
        strcpy( echo_p[usCount].szFindAfter, szFindAfter );

        sprintf( szMessage, " [%d] Adding server \'%s\' to configuration.\n", usCount, echo_p[usCount].szServer );
        message_out( szMessage, MESSAGE_DIAGNOSTIC, usVerbosity );

        if ( ++usCount >= US_MAX_SERVERS ) break;

    }
    message_out("\n", MESSAGE_DIAGNOSTIC, usVerbosity );
    fclose( pfConfig );

    return ( usCount );
}


/* -------------------------------------------------------------------------- *
 * SocketConnect                                                              *
 *                                                                            *
 * Open a TCP/IP socket connection to a remote server.                        *
 *                                                                            *
 * PARAMETERS:                                                                *
 *     PSZ    pszDestIP  : A string containing the IP address or hostname of  *
 *                         the server to connect to.                          *
 *     USHORT usDestPort : The IP port number on the server to connect to,    *
 *                         or 0 to use the default port (80).                 *
 *                                                                            *
 * RETURNS: ULONG                                                             *
 *     One of the following values:                                           *
 *       0        Connection error                                            *
 *       (other)  The connected socket                                        *
 * -------------------------------------------------------------------------- */
int SocketConnect( char *pszDestIP, unsigned short usDestPort, unsigned short usVerbosity )
{
    SOCKADDR_IN destination;
    PHOSTENT    phost;
    char        szMessage[ US_SZMESSAGE_MAX + 1 ];   // output message
    int         ulSocket = 0,
                ulRc;


    // Validate parameters
    if ( pszDestIP  == NULL ) pszDestIP  = SZ_DEFAULT_ECHO_SERVER;
    if ( usDestPort == 0 )    usDestPort = US_DEFAULT_ECHO_PORT;

    sprintf( szMessage, "Attempting connection: %s:%d\n", pszDestIP, usDestPort );
    message_out( szMessage, MESSAGE_VERBOSE, usVerbosity );

    // Create the socket
    ulSocket = socket( PF_INET, SOCK_STREAM, 0 );
    if ( ulSocket < 0 ) {
        psock_errno("Error creating socket");
        return ( 0 );
    }

    // Resolve the hostname
    phost = gethostbyname( pszDestIP );
    if ( phost == NULL ) {
        printf("Could not resolve host \'%s\'.\n\n", pszDestIP );
        return ( 0 );
    }

    // Initialize the connection parameters
    memset( &destination, 0, sizeof(destination) );
    destination.sin_len    = sizeof( destination );
    destination.sin_family = AF_INET;
    destination.sin_port   = htons( usDestPort );
    memcpy( (char *) &destination.sin_addr, phost->h_addr, phost->h_length );

    // Connect the socket
    ulRc = connect( ulSocket, (struct sockaddr *) &destination, sizeof(destination) );
    if ( ulRc != 0 ) {
        psock_errno("Error connecting to server");
        return ( 0 );
    }

    return ( ulSocket );
}


/* -------------------------------------------------------------------------- *
 * SocketRead                                                                 *
 *                                                                            *
 * Attempt to read data from the specified socket, with error checking.       *
 * -------------------------------------------------------------------------- */
short SocketRead( int ulSocket, char *pszResponse )
{
    int  ulRc,
         ulReady,
         ulBytes;
    char *pszData;
    int  readSocks[ 1 ];

    // Check for incoming data on the socket
    readSocks[ 0 ] = ulSocket;
    ulReady = os2_select( readSocks, 1, 0, 0, UL_SELECT_TIMEOUT );

    switch( ulReady ) {
        case -1:    // Socket select returned an error
            psock_errno("Error issuing select()");
            return ( 0 );
            break;

        case 0 :    // Socket select timed out
            printf("Timed out waiting for server to respond.\n");
            return ( 0 );
            break;

        default:    // Socket reports data available
            // Get the size of data to allocate our buffer
            ulRc = ioctl( ulSocket, FIONREAD, &ulBytes );
            if ( ulBytes == 0 ) {
                printf("No data available from server.\n");
                return ( 0 );
            } else if ( ulRc == -1 ) {
                psock_errno("Cannot get data from server");
                return ( 0 );
            }
            ulBytes = ( ulBytes > UL_DATA_LIMIT ) ? UL_DATA_LIMIT : ulBytes;
            pszData = (char *) malloc( ulBytes );

            // Now read the data
            ulRc = recv( ulSocket, pszData, ulBytes, 0 );
            if ( ulRc == -1 ) {
                psock_errno("Error reading socket");
                return ( 0 );
            }
            strncpy( pszResponse, pszData, ulBytes );
            free( pszData );
            break;
    }

    return ( 1 );
}


/* -------------------------------------------------------------------------- *
 * SocketWrite                                                                *
 *                                                                            *
 * Write some data to the specified socket, with error checking.              *
 * -------------------------------------------------------------------------- */
short SocketWrite( int ulSocket, char *pszData )
{
    int ulRc;

    if ( pszData == NULL ) return ( 0 );
    ulRc = send( ulSocket, pszData, strlen(pszData), 0 );
    if ( ulRc < 0 ) {
        psock_errno("Error writing to socket");
        return ( 0 );
    }

    return ( 1 );
}


/* -------------------------------------------------------------------------- *
 * EchoQuery                                                                  *
 *                                                                            *
 * Query the server for our public IP address, and print the result.          *
 * -------------------------------------------------------------------------- */
ULONG EchoQuery( ECHOPARMS echo_p, USHORT usVerbosity )
{
    ULONG  ulRc;
    USHORT usTries = 0;
    CHAR   szCommand[ US_SZCOMMAND_MAX + 1 ],
           szResponse[ US_SZRESPONSE_MAX + 1 ] = "\0";
    PSZ    pszBody, pszOffset;
    int    ip1, ip2, ip3, ip4;
    BOOL   fChunked = FALSE;

    VIOMODEINFO mode;
    USHORT      c, cols;
    CHAR        chSep;

#ifdef USE_HTTP_10
    sprintf( szCommand, "GET http://%s%s HTTP/1.0\r\nAccept: */*\r\n\r\n", echo_p.szServer, echo_p.szDir );
#else
    sprintf( szCommand, "GET %s HTTP/1.1\r\nHost: %s\r\nAccept: */*\r\n\r\n", echo_p.szDir, echo_p.szServer );
#endif
    ulRc = SocketWrite( echo_p.ulSocket, szCommand );
    if ( ulRc == 0 ) return ( 1 );

    while ( ++usTries <= US_MAX_READS ) {
        ulRc = SocketRead( echo_p.ulSocket, szResponse );
        if ( ulRc ) break;
    }
    if ( usTries > US_MAX_READS ) {
        printf("Could not get response from server.  Giving up.\n");
        return ( 1 );
    }

    if ( usVerbosity >= MESSAGE_DIAGNOSTIC ) {
        mode.cb = sizeof(VIOMODEINFO);
        ulRc = VioGetMode( &mode, 0 );
        cols = ( ulRc == NO_ERROR ) ? mode.col - 1 : 0;
        chSep = BoxChar('Ä', '=');
        for ( c = 0; c < cols; c++ ) printf("%c", chSep );
        printf("\nSERVER RESPONSE\n");
        for ( c = 0; c < cols; c++ ) printf("%c", chSep );
        printf("\n%s\n", szResponse );
        for ( c = 0; c < cols; c++ ) printf("%c", chSep );
        printf("\n");
    }

    // Check the transfer encoding as it affects our parsing logic
    if ( strstr( szResponse, "\r\nTransfer-Encoding: chunked\r\n") != NULL )
        fChunked = TRUE;

    // Skip past the first blank line (marking the end of HTTP headers)
    pszBody = strstr( szResponse, "\r\n\r\n");
    pszBody = pszBody ? pszBody+4 : szResponse;

    if ( fChunked ) {
        // Skip the next line (which specifies the chunk size)
        pszBody = strstr( pszBody, "\r\n");
        pszBody = pszBody ? pszBody+2 : szResponse;
    }

    // Look for the "find after" string
    pszOffset = echo_p.szFindAfter[0] ?
                    strstr( pszBody, echo_p.szFindAfter ) :
                    NULL;
    if ( pszOffset == NULL ) pszOffset = pszBody;
    else pszOffset += strlen( echo_p.szFindAfter );

    // Now try and parse the IP address

    ulRc = sscanf( pszOffset, "%3d.%3d.%3d.%3d", &ip1, &ip2, &ip3, &ip4 );
    if ( ulRc != 4 ) {
        printf("Could not parse IP address from server response.\n");
        message_out( pszOffset, MESSAGE_VERBOSE, usVerbosity );
        return ( 1 );
    }
    message_out("Returned IP Address: ", MESSAGE_VERBOSE, usVerbosity );
    printf("%d.%d.%d.%d\n", ip1, ip2, ip3, ip4 );

    return ( NO_ERROR );
}


/* -------------------------------------------------------------------------- *
 * BoxChar                                                                    *
 *                                                                            *
 * Decide if the current codepage is capable of printing the specified box-   *
 * drawing character (single or double line only).  Return the specified      *
 * fallback character if it can't.                                            *
 *                                                                            *
 * PARAMETERS:                                                                *
 *     CHAR  chPref : The preferred box-drawing character.                    *
 *     CHAR  chPlain: The fallback character to use if the codepage can't     *
 *                    handle chPref correctly.                                *
 *                                                                            *
 * RETURNS: CHAR                                                              *
 *     The character to use.                                                  *
 * -------------------------------------------------------------------------- */
CHAR BoxChar( CHAR chPref, CHAR chPlain )
{
    ULONG ulCP[ 3 ],
          ulSize;

    if ( DosQueryCp( sizeof(ulCP), ulCP, &ulSize ) != NO_ERROR )
        return ( chPlain );

    switch ( ulCP[0] ) {
        case 437:
        case 850:
        case 852:
        case 855:
        case 857:
        case 859:
        case 860:
        case 861:
        case 862:
        case 863:
        case 865:
        case 866:
        case 869: return ( chPref );  break;
        default:  break;
    }
    return ( chPlain );
}


/* ------------------------------------------------------------------------- *
 * strstrip                                                                  *
 *                                                                           *
 * A rather quick-and-dirty function to strip leading and trailing white     *
 * space from a string.                                                      *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   PSZ s: The string to be stripped.  This parameter will contain the      *
 *          stripped string when the function returns.                       *
 *                                                                           *
 * RETURNS: PSZ                                                              *
 *   A pointer to the string s.                                              *
 * ------------------------------------------------------------------------- */
char *strstrip( char *s )
{
    int  next,
         last,
         i,
         len,
         newlen;
    char *s2;

    len  = strlen( s );
    next = strspn( s, " \t\n\r");
    for ( i = len - 1; i >= 0; i-- ) {
        if ( s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') break;
    }
    last = i;
    if (( next >= len ) || ( next > last )) {
        memset( s, 0, len+1 );
        return s;
    }

    newlen = last - next + 1;
    s2 = (char *) malloc( newlen + 1 );
    i = 0;
    while ( next <= last )
        s2[i++] = s[next++];
    s2[i] = 0;

    memset( s, 0, len+1 );
    strncpy( s, s2, newlen );
    free( s2 );

    return ( s );
}


/* -------------------------------------------------------------------------- *
 * message_out                                                                *
 *                                                                            *
 * Prints the specified message if its designated verbosity level falls under *
 * the current verbosity level.                                               *
 *                                                                            *
 * ARGUMENTS:                                                                 *
 *   PSZ pszText       : The message to be printed                            *
 *   USHORT usLevel    : The required verbosity of the message                *
 *   USHORT usVerbosity: The actual verbosity in effect                       *
 *                                                                            *
 * RETURNS: n/a                                                               *
 * -------------------------------------------------------------------------- */
void message_out( PSZ pszText, USHORT usLevel, USHORT usVerbosity )
{
    if ( usVerbosity >= usLevel ) printf("%s", pszText );
}

