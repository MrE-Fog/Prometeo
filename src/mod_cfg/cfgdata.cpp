/***************************************************************************
                                 cfgdata.cpp
                             -------------------
	revision             : $Id: cfgdata.cpp,v 1.1.1.1 2002-10-10 09:59:25 tellini Exp $
    copyright            : (C) 2002 by Simone Tellini
    email                : tellini@users.sourceforge.net

	description          : configuration editor via web
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "main.h"

#include <fstream>
#include <stdio.h>

#include "tcpsocket.h"
#include "mystring.h"
#include "stringlist.h"
#include "registry.h"
#include "loader.h"

#include "cfgdata.h"
#include "base64.h"
#include "pagemaker.h"

#define TIMEOUT_REQUEST				15
#define TIMEOUT_BODY				45

#define HTTP_OK                     200
#define HTTP_MOVED_TEMPORARILY      302
#define HTTP_BAD_REQUEST            400
#define HTTP_UNAUTHORIZED			401
#define HTTP_FORBIDDEN              403
#define HTTP_NOT_FOUND              404
#define HTTP_INTERNAL_ERROR         500
#define HTTP_NOT_IMPLEMENTED        501
#define HTTP_BAD_GATEWAY            502

static const struct
{
    int     	code;
    const char	*title;
}   Codes[] = {
        { 200, "OK" },
        { 201, "Created" },
        { 202, "Accepted" },
        { 203, "Non-Authoritative Information" },
        { 204, "No Content" },
        { 205, "Reset Content" },
        { 206, "Partial Content" },
        { 300, "Multiple Choices" },
        { 301, "Moved Permanently" },
        { 302, "Moved Temporarily" },
        { 303, "See Other" },
        { 304, "Not Modified" },
        { 305, "Use Proxy" },
        { 400, "Bad Request" },
        { 401, "Unauthorized" },
        { 402, "Payment Required" },
        { 403, "Forbidden" },
        { 404, "Not Found" },
        { 405, "Method Not Allowed" },
        { 406, "Not Acceptable" },
        { 407, "Proxy Authentication Required" },
        { 408, "Request Timeout" },
        { 409, "Conflict" },
        { 410, "Gone" },
        { 411, "Length Required" },
        { 412, "Precondition Failed" },
        { 413, "Request Entity Too Large" },
        { 414, "Request-URI Too Long" },
        { 415, "Unsupported Media Type" },
        { 500, "Internal Server Error" },
        { 501, "Not Implemented" },
        { 502, "Bad Gateway" },
        { 503, "Service Unavailable" },
        { 504, "Gateway Timeout" },
        { 505, "HTTP Version Not Supported" },
};

#define NUM_CODES   ( sizeof( Codes ) / sizeof( Codes[0] ))

//---------------------------------------------------------------------------
static void SocketCB( SOCKREF sock, Prom_SC_Reason reason, int data, void *userdata )
{
	((CfgData *)userdata )->SocketEvent( reason, data );
}
//---------------------------------------------------------------------------
CfgData::CfgData( CfgEditor *cfg, TcpSocket *sock )
{
	Editor  = cfg;
	Peer    = sock;
	State   = S_REQUEST_HEADER;
	Method  = M_NONE;
	ReqLen  = 0;
	BodyLen = 0;

	Peer->UseDispatcher( App->IO );
	Peer->SetAsyncCallback( SocketCB, this );
	Peer->AsyncRecv( ReqBuf, sizeof( ReqBuf ) - 1, 0, TIMEOUT_REQUEST );
}
//---------------------------------------------------------------------------
CfgData::~CfgData()
{
	delete Peer;
}
//---------------------------------------------------------------------------
void CfgData::SocketEvent( Prom_SC_Reason reason, int data )
{
		switch( reason ) {

		case PROM_SOCK_ERROR:
		case PROM_SOCK_TIMEOUT:
			Error( data );
			break;

		case PROM_SOCK_READ:
			GotData( data );
			break;

		case PROM_SOCK_WRITTEN:
			if( State == S_CLOSING )
				delete this;
			break;
	}
}
//---------------------------------------------------------------------------
void CfgData::Error( int err )
{
	App->Log->Log( LOG_ERR,
				   "mod_cfg: error while serving %s - %s",
				   Peer->GetPeerName(),
				   err ? strerror( err ) : "timeout" );

	delete this;
}
//---------------------------------------------------------------------------
void CfgData::GotData( int len )
{
	switch( State ) {

		case S_REQUEST_HEADER:
			AddHeaderData( len );
			break;

		case S_REQUEST_BODY:
			AddBodyData( len );
			break;
	}
}
//---------------------------------------------------------------------------
void CfgData::AddHeaderData( int len )
{
	char *end;

	ReqLen           += len;
	ReqBuf[ ReqLen ]  = '\0';

	if( end = strchr( ReqBuf, '\n' )) {
		char *start = ReqBuf;

		do {

			*end = '\0';

			if(( end > start ) && ( end[ -1 ] == '\r' ))
				end[ -1 ] = '\0';

			if( Method == M_NONE )
				ParseMethod();
			else
				ParseHeader( start );

			start = end + 1;

		} while(( State == S_REQUEST_HEADER ) && ( end = strchr( start, '\n' )));

		if( State == S_REQUEST_BODY ) {
			int len = ReqLen - ( start - ReqBuf );

			// copy the left-overs in the right buffer
			memcpy( Body.GetData(), start, len );
			
			AddBodyData( len );
			
		} else if( State == S_REQUEST_HEADER )
			Peer->AsyncRecv( ReqBuf, sizeof( ReqBuf ) - 1, 0, TIMEOUT_BODY );

	} else if( ReqLen < sizeof( ReqBuf ) - 1 )
		Peer->AsyncRecv( &ReqBuf[ ReqLen ], sizeof( ReqBuf ) - ReqLen - 1, 0, TIMEOUT_BODY );

	else
		SendError( HTTP_BAD_REQUEST, "Request header is too long." );
}
//---------------------------------------------------------------------------
void CfgData::AddBodyData( int len )
{
	BodyLen += len;

	if( BodyLen >= Body.GetSize() - 1 ) {
		char *ptr = Body.GetData() + BodyLen;
	
		*ptr-- = '\0';

		if( *ptr == '\n' ) {
				
			*ptr-- = '\0';

			if(( BodyLen > 1 ) && ( *ptr == '\r' ))
				*ptr = '\0';
		}

		ParseRequest();

	} else
		Peer->AsyncRecv( Body.GetData() + BodyLen, Body.GetSize() - BodyLen - 1,
						 0, TIMEOUT_BODY );
}
//---------------------------------------------------------------------------
void CfgData::ParseMethod( void )
{
	char	*ptr;

	if( char *url = strchr( ReqBuf, ' ' )) {

		*url++ = '\0';

		if( ptr = strchr( url, ' ' ))
			*ptr = '\0';

		if(( *url++ == '/' ) && !strstr( url, "../" )) {
			char	*query = strchr( url, '?' );

			if( query ) {
				*query++    = '\0';
				QueryString = query;
			}

			RequestedPage = url;

			if( !strcmp( ReqBuf, "POST" ))
				Method = M_POST;
			else if( !strcmp( ReqBuf, "GET" ))
				Method = M_GET;
			else
				SendError( HTTP_NOT_IMPLEMENTED, "Method not implemented." );

		} else
			SendError( HTTP_FORBIDDEN, "Sorry, you can't request that." );

	} else
		SendError( HTTP_BAD_REQUEST, "Your browser sent a malformed request." );
}
//---------------------------------------------------------------------------
void CfgData::ParseHeader( char *hdr )
{
	char	*ptr = hdr, *args = NULL;
	bool	needargs = false;

	while( *ptr && !args ) {

		*ptr = tolower( *ptr );

		if( needargs ) {

			if( !args && ( *ptr != ' ' ))
				args = ptr;

		} else if( *ptr == ':' ) {

			*ptr     = '\0';
			needargs = true;
		}

		ptr++;
	}

	if( !args )
		args = "";

	if( !hdr[0] ) {

		if(( Method == M_GET ) || !Body.GetSize() )
			ParseRequest();
		else
			State = S_REQUEST_BODY;

	} else if( !strcmp( hdr, "content-length" )) {
		int len = atoi( args ) + 1; // + 1 for '\0'

		// cap it to something sane to avoid DoS
		if( len > ( 1 * 1024 * 1024 ))
			len = ( 1 * 1024 * 1024 );

		Body.Resize( len );

	} else if( !strcmp( hdr, "authorization" ))
		ParseAuth( args );
}
//---------------------------------------------------------------------------
void CfgData::ParseRequest( void )
{
	if( RequestedPage == "" )
		RequestedPage = "general";

	if( IsAuthorized() ) {

		if( RequestedPage.substr( 0, 6 ) == "files/" )
			SendFile( RequestedPage.substr( 5 ));

		else {
			PageMaker	pg;
			string		body;
			int			len;

			// if we're asked for a module configuration page, we need
			// to load the right manifest
			if( RequestedPage.substr( 0, 4 ) == "mod/" ) {
				string				mod;
				string::size_type	pos;

				RequestedPage = UrlDecode( RequestedPage );
				mod           = RequestedPage.substr( 4 );
				pos           = mod.find( "/" );

				if( pos != string::npos )
					mod.erase( pos );

				pg.SetModuleManifest( App->Mods->GetManifest( mod.c_str() ));
			}

			ProcessRequest( pg );

			pg.BuildPage( RequestedPage, body );

			len = body.length();

			if( len > 0 ) {

				Peer->AsyncPrintf( "HTTP/1.0 200 OK\r\n"
								   "Server: "PACKAGE"/"VERSION" mod_cfg/"MOD_VERSION"\r\n"
								   "Connection: close\r\n"
								   "Content-Type: text/html\r\n"
								   "Content-Length: %d\r\n"
								   "Cache-Control: no-cache, no-store, private, must-revalidate\r\n"
								   "Pragma: no-cache\r\n"
								   "\r\n",
								   len );

				Peer->AsyncSend( body.c_str(), len );

				State = S_CLOSING;

			} else
				SendError( HTTP_INTERNAL_ERROR, "Cannot create the page." );
		}

	} else
		SendError( HTTP_UNAUTHORIZED, "You need to authenticate to pass this point." );
}
//---------------------------------------------------------------------------
void CfgData::ProcessRequest( PageMaker& pg )
{
	if( RequestedPage == "apply" ) {

		App->Cfg->Save();
		App->CfgReload();

	} else if( RequestedPage == "deletemod" )
		DeleteMod();

	else if( RequestedPage == "addmod" )
		AddMod();

	else if( RequestedPage == "enablemod" )
		EnableMod( true );

	else if( RequestedPage == "disablemod" )
		EnableMod( false );

	else if( RequestedPage == "loadmod" )
		LoadMod( true );

	else if( RequestedPage == "unloadmod" )
		LoadMod( false );

	else if( Method == M_POST )
		UpdateSettings( pg );
}
//---------------------------------------------------------------------------
const char *CfgData::GetCodeMsg( int code ) const
{
	for( int i = 0; i < NUM_CODES; i++ )
		if( Codes[ i ].code == code )
			return( Codes[ i ].title );

	return( "" );
}
//---------------------------------------------------------------------------
void CfgData::SendError( int code, const char *body )
{
	const char *msg = GetCodeMsg( code ), *extra = "";

	State = S_CLOSING;

	if( code == HTTP_UNAUTHORIZED )
		extra = "WWW-Authenticate: Basic realm=\"prometeo's administrative interface\"\r\n";

	Peer->AsyncPrintf( "HTTP/1.0 %d %s\r\n"
					   "Server: "PACKAGE"/"VERSION" mod_cfg/"MOD_VERSION"\r\n"
					   "Connection: close\r\n"
					   "Content-Type: text/html\r\n"
					   "%s"
					   "\r\n"
					   "<html>"
					   "<head><title>Prometeo/mod_cfg - Error</title></head>"
					   "<body>"
					   "<h2>%d %s</h2>"
					   "%s"
					   "</body>"
					   "</html>",
					   code, msg,
					   extra,
					   code, msg,
					   body );
}
//---------------------------------------------------------------------------
void CfgData::ParseAuth( char *str )
{
	char	*ptr = str;

	while( *ptr && ( *ptr != ' ' ))
		*ptr++ = tolower( *ptr );

	*ptr++ = '\0';

	if( !strcmp( str, "basic" )) {
		char buf[512];

		Base64Decode( ptr, buf, sizeof( buf ));

		if( ptr = strchr( buf, ':' )) {

			*ptr++ = '\0';

			Username = buf;
			Password = ptr;
		}
	}
}
//---------------------------------------------------------------------------
bool CfgData::IsAuthorized( void )
{
	bool ret = true;

	return( ret );
}
//---------------------------------------------------------------------------
void CfgData::SendFile( string name )
{
	static const struct {
		const char	*Ext;
		const char	*Type;
	} MIMETypes[] = {
		{ "gif", 	"image/gif" },
		{ "png", 	"image/png" },
		{ "jpg", 	"image/jpeg" },
		{ "css", 	"text/css" },
		{ "js",		"text/javascript" },
	};

#define NUM_TYPES	( sizeof( MIMETypes ) / sizeof( MIMETypes[0] ))

	name = DATA_DIR + name;

	ifstream	fh( name.c_str(), ios::binary );

	if( fh ) {
		string::size_type	p = name.rfind( "." );
		const char			*type = "text/html";

		if( p != string::npos ) {
			string	ext = name.substr( p + 1 );

			for( int i = 0; i < NUM_TYPES; i++ )
				if( ext == MIMETypes[ i ].Ext ) {
					type = MIMETypes[ i ].Type;
					break;
				}
		}

		Peer->AsyncPrintf( "HTTP/1.0 200 OK\r\n"
						   "Server: "PACKAGE"/"VERSION" mod_cfg/"MOD_VERSION"\r\n"
						   "Connection: close\r\n"
						   "Content-Type: %s\r\n"
						   "\r\n",
						   type );

		while( !fh.eof() ) {
			char	buf[ 1024 ];
			int		num;

			fh.read( buf, sizeof( buf ));

			Peer->AsyncSend( buf, fh.gcount() );
		}

		State = S_CLOSING;

		fh.close();

	} else
		SendError( HTTP_NOT_FOUND, "File not found." );
}
//---------------------------------------------------------------------------
string CfgData::DecodeArg( const char *arg )
{
	string				val, a;
	string::size_type	pos;

	val  = Body.GetData();
	a    = arg;
	a   += "=";

	pos = val.find( a );

	if( pos != string::npos ) {

		val = val.substr( pos + a.length() );
		pos = val.find( "&" );

		if( pos != string::npos )
			val = val.substr( 0, pos );

		val = UrlDecode( val );

	} else
		val = "";

	return( val );
}
//---------------------------------------------------------------------------
string CfgData::UrlDecode( const string val )
{
	const char	*src;
	char		*dst, *tmp;
	string		ret;

	tmp = new char[ val.length() + 1 ];
	src = val.c_str();
	dst = tmp;

	while( *src ) {
		char ch = *src++;

		switch( ch ) {

			case '+':
				*dst++ = ' ';
				break;

			case '%': // %xx - hex value to decode
				char v1, v2;

				if(( v1 = *src ) && ( v2 = *++src )) {
					static const char 	hex[] = "0123456789abcdef";
					const char			*ptr;

					if( ptr = strchr( hex, tolower( v1 ))) {

						v1 = ptr - hex;

						if( ptr = strchr( hex, tolower( v2 ))) {

							v2 = ptr - hex;

							*dst++ = ( v1 << 4 ) | v2;
						}
					}

					src++;
				}
				break;

			default:
				*dst++ = ch;
				break;
		}
	}

	*dst = '\0';

	ret = tmp;

	delete[] tmp;

	return( ret );
}
//---------------------------------------------------------------------------
void CfgData::UpdateSettings( PageMaker& pg )
{
	if( RequestedPage == "mods" )
		UpdateMods( pg );
	else {
		string		page = RequestedPage;

		if( page.substr( 0, 4 ) == "mod/" )
			page.erase( 0, 4 );

		StringList&	options = pg.GetOptions( page );

		for( int i = 0; i < options.Count(); i++ ) {
			MyString			str = options[ i ];
			StringList			list;
			string::size_type	pos;

			str.Explode( "|", list );

			str = list[ OP_KEY ];

			pos = str.rfind( "/" );

			if( pos != string::npos ) {
				string key, node, val;

				key  = str.substr( 0, pos );
				node = str.substr( pos + 1 );
				val  = DecodeArg( list[ OP_NAME ] );

				if( App->Cfg->OpenKey( key.c_str(), true )) {

					if( !strcmp( list[ OP_TYPE ], "string" ))
						App->Cfg->SetString( node.c_str(), val.c_str() );

					else if( !strcmp( list[ OP_TYPE ], "integer" ))
						App->Cfg->SetInteger( node.c_str(), atoi( val.c_str() ));

					else if( !strcmp( list[ OP_TYPE ], "bool" ))
						App->Cfg->SetInteger( node.c_str(), atoi( val.c_str() ));

					App->Cfg->CloseKey();
				}
			}
		}
	}
}
//---------------------------------------------------------------------------
void CfgData::UpdateMods( PageMaker& pg )
{
	if( App->Cfg->OpenKey( "root/Modules", false )) {

		App->Cfg->SetString( "path", DecodeArg( "path" ).c_str() );

		App->Cfg->CloseKey();
	}
}
//---------------------------------------------------------------------------
void CfgData::DeleteMod( void )
{
	string	mod = DecodeArg( "mod" );

	App->Mods->Unload( mod.c_str() );

	mod.insert( 0, "root/Modules/" );

	App->Cfg->DeleteKey( mod.c_str() );

	RequestedPage = "mods";
}
//---------------------------------------------------------------------------
void CfgData::AddMod( void )
{
	string	name, mod;
	bool	enabled;

	name    = "root/Modules/" + DecodeArg( "name" );
	mod     = DecodeArg( "file" );
	enabled = DecodeArg( "enabled" ) == "on";

	if( App->Cfg->OpenKey( name.c_str(), true )) {

		App->Cfg->SetString( "module", mod.c_str() );
		App->Cfg->SetInteger( "enabled", enabled );

		App->Cfg->CloseKey();
	}

	RequestedPage = "mods";
}
//---------------------------------------------------------------------------
void CfgData::EnableMod( bool enable )
{
	string	name;

	name = "root/Modules/" + DecodeArg( "mod" );

	if( App->Cfg->OpenKey( name.c_str(), true )) {

		App->Cfg->SetInteger( "enabled", enable );

		App->Cfg->CloseKey();
	}

	RequestedPage = "mods";
}
//---------------------------------------------------------------------------
void CfgData::LoadMod( bool load )
{
	string mod = DecodeArg( "mod" );

	if( load ) {

		if( !App->Mods->IsRunning( mod.c_str() ))
			App->Mods->Load( mod.c_str(), true );

	} else
		App->Mods->Unload( mod.c_str() );

	RequestedPage = "mods";
}
//---------------------------------------------------------------------------