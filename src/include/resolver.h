/***************************************************************************
                                 resolver.h
                             -------------------
	revision             : $Id: resolver.h,v 1.1.1.1 2002-10-10 09:59:16 tellini Exp $
    copyright            : (C) 2002 by Simone Tellini
    email                : tellini@users.sourceforge.net

	description          : a DNS resolver class. It spawns a new process
	                       to handle dns queries asynchronously
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef RESOLVER_H
#define RESOLVER_H

#include "process.h"

struct ResolverRequest
{
	int		Family;			// AF_INET or AF_INET6
	char	HostName[0];	// actually, its size is strlen( HostName ) + 1
};

class Resolver : public Process
{
public:
	virtual void	OnFork( void );
		
private:
	virtual void	Dispatch( void );
};

#endif