/***************************************************************************
              targetlistcomponent.cpp  -  K Desktop Planetarium
                             -------------------
    begin                : Oct 14 2010 9:59 PM CDT
    copyright            : (C) 2010 Akarsh Simha
    email                : akarsh.simha@kdemail.net
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "targetlistcomponent.h"

#ifndef KSTARS_LITE
#include "skymap.h"
#endif
#include "skypainter.h"

TargetListComponent::TargetListComponent(SkyComposite *parent) : SkyComponent(parent)
{
    drawSymbols = 0;
    drawLabels  = 0;
}

TargetListComponent::TargetListComponent(SkyComposite *parent, SkyObjectList *objectList, QPen _pen,
                                         bool (*optionDrawSymbols)(void), bool (*optionDrawLabels)(void))
    : SkyComponent(parent), list(objectList), pen(_pen)
{
    drawSymbols = optionDrawSymbols;
    drawLabels  = optionDrawLabels;
}

TargetListComponent::~TargetListComponent()
{
    if (list.get())
    {
        qDeleteAll(*list);
    }
}

void TargetListComponent::draw(SkyPainter *skyp)
{
    if (drawSymbols && !(*drawSymbols)())
        return;
    if (!list || list->count() <= 0)
        return;

    skyp->setPen(pen);
    skyp->drawObservingList(*list);
}
