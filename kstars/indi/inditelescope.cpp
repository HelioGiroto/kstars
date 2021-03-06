/*  INDI CCD
    Copyright (C) 2012 Jasem Mutlaq <mutlaqja@ikarustech.com>

    This application is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
 */

#include "inditelescope.h"

#include "clientmanager.h"
#include "driverinfo.h"
#include "indidevice.h"
#include "kstars.h"
#include "Options.h"
#include "skymap.h"
#include "skymapcomposite.h"

#include <KNotification>

#include <indi_debug.h>

namespace ISD
{
Telescope::Telescope(GDInterface *iPtr) : DeviceDecorator(iPtr)
{
    dType                = KSTARS_TELESCOPE;
    minAlt               = -1;
    maxAlt               = -1;
    EqCoordPreviousState = IPS_IDLE;

    centerLockTimer = new QTimer(this);
    // Set it for 5 seconds for now as not to spam the display update
    centerLockTimer->setInterval(5000);
    centerLockTimer->setSingleShot(true);
    connect(centerLockTimer, &QTimer::timeout, this, [this]() { runCommand(INDI_CENTER_LOCK); });
}

Telescope::~Telescope()
{
}

void Telescope::registerProperty(INDI::Property *prop)
{
    if (!strcmp(prop->getName(), "TELESCOPE_INFO"))
    {
        INumberVectorProperty *ti = prop->getNumber();

        if (ti == nullptr)
            return;

        bool aperture_ok = false, focal_ok = false;
        double temp = 0;

        INumber *aperture = IUFindNumber(ti, "TELESCOPE_APERTURE");
        if (aperture && aperture->value == 0)
        {
            if (getDriverInfo()->getAuxInfo().contains("TELESCOPE_APERTURE"))
            {
                temp = getDriverInfo()->getAuxInfo().value("TELESCOPE_APERTURE").toDouble(&aperture_ok);
                if (aperture_ok)
                {
                    aperture->value     = temp;
                    INumber *g_aperture = IUFindNumber(ti, "GUIDER_APERTURE");
                    if (g_aperture && g_aperture->value == 0)
                        g_aperture->value = aperture->value;
                }
            }
        }

        INumber *focal_length = IUFindNumber(ti, "TELESCOPE_FOCAL_LENGTH");
        if (focal_length && focal_length->value == 0)
        {
            if (getDriverInfo()->getAuxInfo().contains("TELESCOPE_FOCAL_LENGTH"))
            {
                temp = getDriverInfo()->getAuxInfo().value("TELESCOPE_FOCAL_LENGTH").toDouble(&focal_ok);
                if (focal_ok)
                {
                    focal_length->value = temp;
                    INumber *g_focal    = IUFindNumber(ti, "GUIDER_FOCAL_LENGTH");
                    if (g_focal && g_focal->value == 0)
                        g_focal->value = focal_length->value;
                }
            }
        }

        if (aperture_ok && focal_ok)
            clientManager->sendNewNumber(ti);
    }

    if (!strcmp(prop->getName(), "TELESCOPE_PARK"))
    {
        ISwitchVectorProperty *svp = prop->getSwitch();

        if (svp)
        {
            ISwitch *sp = IUFindSwitch(svp, "PARK");
            if (sp)
            {
                if ((sp->s == ISS_ON) && svp->s == IPS_OK)
                    parkStatus = PARK_PARKED;
                else if ((sp->s == ISS_OFF) && svp->s == IPS_OK)
                    parkStatus = PARK_UNPARKED;
            }
        }
    }

    if (!strcmp(prop->getName(), "ALIGNMENT_POINTSET_ACTION") || !strcmp(prop->getName(), "ALIGNLIST"))
        m_hasAlignmentModel = true;

    DeviceDecorator::registerProperty(prop);
}

void Telescope::processNumber(INumberVectorProperty *nvp)
{
    if (!strcmp(nvp->name, "EQUATORIAL_EOD_COORD"))
    {
        INumber *RA  = IUFindNumber(nvp, "RA");
        INumber *DEC = IUFindNumber(nvp, "DEC");

        if (RA == nullptr || DEC == nullptr)
            return;

        currentCoord.setRA(RA->value);
        currentCoord.setDec(DEC->value);
        currentCoord.EquatorialToHorizontal(KStars::Instance()->data()->lst(),
                                            KStars::Instance()->data()->geo()->lat());

        if (nvp->s == IPS_BUSY && EqCoordPreviousState != IPS_BUSY)
        {
            if (getStatus() != MOUNT_PARKING)
                KNotification::event(QLatin1String("SlewStarted"), i18n("Mount is slewing to target location"));
        }
        else if (EqCoordPreviousState == IPS_BUSY && nvp->s == IPS_OK)
        {
            KNotification::event(QLatin1String("SlewCompleted"), i18n("Mount arrived at target location"));

            double maxrad = 1000.0 / Options::zoomFactor();

            currentObject = KStarsData::Instance()->skyComposite()->objectNearest(&currentCoord, maxrad);
            if (currentObject != nullptr)
                emit newTarget(currentObject->name());
        }

        EqCoordPreviousState = nvp->s;

        KStars::Instance()->map()->update();
    }
    else if (!strcmp(nvp->name, "HORIZONTAL_COORD"))
    {
        INumber *Az  = IUFindNumber(nvp, "AZ");
        INumber *Alt = IUFindNumber(nvp, "ALT");

        if (Az == nullptr || Alt == nullptr)
            return;

        currentCoord.setAz(Az->value);
        currentCoord.setAlt(Alt->value);
        currentCoord.HorizontalToEquatorial(KStars::Instance()->data()->lst(),
                                            KStars::Instance()->data()->geo()->lat());

        KStars::Instance()->map()->update();
    }

    DeviceDecorator::processNumber(nvp);
}

void Telescope::processSwitch(ISwitchVectorProperty *svp)
{
    bool manualMotionChanged = false;

    if (!strcmp(svp->name, "TELESCOPE_PARK"))
    {
        ISwitch *sp = IUFindSwitch(svp, "PARK");
        if (sp)
        {
            if (svp->s == IPS_ALERT)
            {
                // If alert, set park status to whatever it was opposite to. That is, if it was parking and failed
                // then we set status to unparked since it did not successfully complete parking.
                if (parkStatus == PARK_PARKING)
                    parkStatus = PARK_UNPARKED;
                else if (parkStatus == PARK_UNPARKING)
                    parkStatus = PARK_PARKED;

                KNotification::event(QLatin1String("MountParkingFailed"), i18n("Mount parking failed"));
            }
            else if (svp->s == IPS_BUSY && sp->s == ISS_ON && parkStatus != PARK_PARKING)
            {
                parkStatus = PARK_PARKING;
                KNotification::event(QLatin1String("MountParking"), i18n("Mount parking is in progress"));
                currentObject = nullptr;
            }
            else if (svp->s == IPS_BUSY && sp->s == ISS_OFF && parkStatus != PARK_UNPARKING)
            {
                parkStatus = PARK_UNPARKING;
                KNotification::event(QLatin1String("MountUnParking"), i18n("Mount unparking is in progress"));
            }
            else if (svp->s == IPS_OK && sp->s == ISS_ON && parkStatus != PARK_PARKED)
            {
                parkStatus = PARK_PARKED;
                KNotification::event(QLatin1String("MountParked"), i18n("Mount parked"));
                currentObject = nullptr;
            }
            else if (svp->s == IPS_OK && sp->s == ISS_OFF && parkStatus != PARK_UNPARKED)
            {
                parkStatus = PARK_UNPARKED;
                KNotification::event(QLatin1String("MountUnparked"), i18n("Mount unparked"));
                currentObject = nullptr;
            }
        }
    }
    else if (!strcmp(svp->name, "TELESCOPE_ABORT_MOTION"))
    {
        if (svp->s == IPS_OK)
        {
            KNotification::event(QLatin1String("MountAborted"), i18n("Mount motion was aborted"));
        }
    }
    else if (!strcmp(svp->name, "TELESCOPE_MOTION_NS"))
        manualMotionChanged = true;
    else if (!strcmp(svp->name, "TELESCOPE_MOTION_WE"))
        manualMotionChanged = true;

    if (manualMotionChanged)
    {
        IPState NSCurrentMotion, WECurrentMotion;

        NSCurrentMotion = baseDevice->getSwitch("TELESCOPE_MOTION_NS")->s;
        WECurrentMotion = baseDevice->getSwitch("TELESCOPE_MOTION_WE")->s;

        if (NSCurrentMotion == IPS_BUSY || WECurrentMotion == IPS_BUSY || NSPreviousState == IPS_BUSY ||
            WEPreviousState == IPS_BUSY)
        {
            if (inManualMotion == false && ((NSCurrentMotion == IPS_BUSY && NSPreviousState != IPS_BUSY) ||
                                            (WECurrentMotion == IPS_BUSY && WEPreviousState != IPS_BUSY)))
            {
                inManualMotion = true;
                KNotification::event(QLatin1String("MotionStarted"), i18n("Mount is manually moving"));
            }
            else if (inManualMotion && ((NSCurrentMotion != IPS_BUSY && NSPreviousState == IPS_BUSY) ||
                                        (WECurrentMotion != IPS_BUSY && WEPreviousState == IPS_BUSY)))
            {
                inManualMotion = false;
                KNotification::event(QLatin1String("MotionStopped"), i18n("Mount is manually moving"));
            }

            NSPreviousState = NSCurrentMotion;
            WEPreviousState = WECurrentMotion;
        }
    }

    DeviceDecorator::processSwitch(svp);
}

void Telescope::processText(ITextVectorProperty *tvp)
{
    DeviceDecorator::processText(tvp);
}

bool Telescope::canGuide()
{
    INumberVectorProperty *raPulse  = baseDevice->getNumber("TELESCOPE_TIMED_GUIDE_WE");
    INumberVectorProperty *decPulse = baseDevice->getNumber("TELESCOPE_TIMED_GUIDE_NS");

    if (raPulse && decPulse)
        return true;
    else
        return false;
}

bool Telescope::canSync()
{
    ISwitchVectorProperty *motionSP = baseDevice->getSwitch("ON_COORD_SET");

    if (motionSP == nullptr)
        return false;

    ISwitch *syncSW = IUFindSwitch(motionSP, "SYNC");

    return (syncSW != nullptr);
}

bool Telescope::canPark()
{
    ISwitchVectorProperty *parkSP = baseDevice->getSwitch("TELESCOPE_PARK");

    if (parkSP == nullptr)
        return false;

    ISwitch *parkSW = IUFindSwitch(parkSP, "PARK");

    return (parkSW != nullptr);
}

bool Telescope::isSlewing()
{
    INumberVectorProperty *EqProp = baseDevice->getNumber("EQUATORIAL_EOD_COORD");

    if (EqProp == nullptr)
        return false;

    return (EqProp->s == IPS_BUSY);
}

bool Telescope::isInMotion()
{
    return (isSlewing() || inManualMotion);
}

bool Telescope::doPulse(GuideDirection ra_dir, int ra_msecs, GuideDirection dec_dir, int dec_msecs)
{
    if (canGuide() == false)
        return false;

    bool raOK = false, decOK = false;
    raOK  = doPulse(ra_dir, ra_msecs);
    decOK = doPulse(dec_dir, dec_msecs);

    if (raOK && decOK)
        return true;
    else
        return false;
}

bool Telescope::doPulse(GuideDirection dir, int msecs)
{
    INumberVectorProperty *raPulse  = baseDevice->getNumber("TELESCOPE_TIMED_GUIDE_WE");
    INumberVectorProperty *decPulse = baseDevice->getNumber("TELESCOPE_TIMED_GUIDE_NS");
    INumberVectorProperty *npulse   = nullptr;
    INumber *dirPulse               = nullptr;

    if (raPulse == nullptr || decPulse == nullptr)
        return false;

    switch (dir)
    {
        case RA_INC_DIR:
            dirPulse = IUFindNumber(raPulse, "TIMED_GUIDE_W");
            if (dirPulse == nullptr)
                return false;

            npulse = raPulse;
            break;

        case RA_DEC_DIR:
            dirPulse = IUFindNumber(raPulse, "TIMED_GUIDE_E");
            if (dirPulse == nullptr)
                return false;

            npulse = raPulse;
            break;

        case DEC_INC_DIR:
            dirPulse = IUFindNumber(decPulse, "TIMED_GUIDE_N");
            if (dirPulse == nullptr)
                return false;

            npulse = decPulse;
            break;

        case DEC_DEC_DIR:
            dirPulse = IUFindNumber(decPulse, "TIMED_GUIDE_S");
            if (dirPulse == nullptr)
                return false;

            npulse = decPulse;
            break;

        default:
            return false;
    }

    dirPulse->value = msecs;

    clientManager->sendNewNumber(npulse);

    //qDebug() << "Sending pulse for " << npulse->name << " in direction " << dirPulse->name << " for " << msecs << " ms " << endl;

    return true;
}

bool Telescope::runCommand(int command, void *ptr)
{
    //qDebug() << "Telescope run command is called!!!" << endl;

    switch (command)
    {
        case INDI_SEND_COORDS:
            if (ptr == nullptr)
                sendCoords(KStars::Instance()->map()->clickedPoint());
            else
                sendCoords(static_cast<SkyPoint *>(ptr));

            break;

        case INDI_ENGAGE_TRACKING:
        {
            SkyPoint J2000Coord(currentCoord.ra(), currentCoord.dec());
            J2000Coord.apparentCoord(KStars::Instance()->data()->ut().djd(), (long double)J2000);
            currentCoord.setRA0(J2000Coord.ra());
            currentCoord.setDec0(J2000Coord.dec());
            KStars::Instance()->map()->setDestination(currentCoord);
        }
        break;

        case INDI_CENTER_LOCK:
        {
            //if (currentObject == nullptr || KStars::Instance()->map()->focusObject() != currentObject)
            if (Options::isTracking() == false ||
                currentCoord.angularDistanceTo(KStars::Instance()->map()->focus()).Degrees() > 0.5)
            {
                SkyPoint J2000Coord(currentCoord.ra(), currentCoord.dec());
                J2000Coord.apparentCoord(KStars::Instance()->data()->ut().djd(), (long double)J2000);
                currentCoord.setRA0(J2000Coord.ra());
                currentCoord.setDec0(J2000Coord.dec());
                //KStars::Instance()->map()->setClickedPoint(&currentCoord);
                //KStars::Instance()->map()->slotCenter();
                KStars::Instance()->map()->setDestination(currentCoord);
                KStars::Instance()->map()->setFocusPoint(&currentCoord);
                //KStars::Instance()->map()->setFocusObject(currentObject);
                KStars::Instance()->map()->setFocusObject(nullptr);
                Options::setIsTracking(true);
            }
            centerLockTimer->start();
        }
        break;

        case INDI_CENTER_UNLOCK:
            KStars::Instance()->map()->stopTracking();
            centerLockTimer->stop();
            break;

        default:
            return DeviceDecorator::runCommand(command, ptr);
            break;
    }

    return true;
}

bool Telescope::sendCoords(SkyPoint *ScopeTarget)
{
    INumber *RAEle                 = nullptr;
    INumber *DecEle                = nullptr;
    INumber *AzEle                 = nullptr;
    INumber *AltEle                = nullptr;
    INumberVectorProperty *EqProp  = nullptr;
    INumberVectorProperty *HorProp = nullptr;
    double currentRA = 0, currentDEC = 0, currentAlt = 0, currentAz = 0, targetAlt = 0;
    bool useJ2000(false);

    EqProp = baseDevice->getNumber("EQUATORIAL_EOD_COORD");
    if (EqProp == nullptr)
    {
        // J2000 Property
        EqProp = baseDevice->getNumber("EQUATORIAL_COORD");
        if (EqProp)
            useJ2000 = true;
    }

    HorProp = baseDevice->getNumber("HORIZONTAL_COORD");

    if (EqProp && EqProp->p == IP_RO)
        EqProp = nullptr;

    if (HorProp && HorProp->p == IP_RO)
        HorProp = nullptr;

    //qDebug() << "Skymap click - RA: " << scope_target->ra().toHMSString() << " DEC: " << scope_target->dec().toDMSString();

    if (EqProp)
    {
        RAEle = IUFindNumber(EqProp, "RA");
        if (!RAEle)
            return false;
        DecEle = IUFindNumber(EqProp, "DEC");
        if (!DecEle)
            return false;

        if (useJ2000)
            ScopeTarget->apparentCoord(KStars::Instance()->data()->ut().djd(), (long double)J2000);

        currentRA  = RAEle->value;
        currentDEC = DecEle->value;

        ScopeTarget->EquatorialToHorizontal(KStarsData::Instance()->lst(), KStarsData::Instance()->geo()->lat());
    }

    if (HorProp)
    {
        AzEle = IUFindNumber(HorProp, "AZ");
        if (!AzEle)
            return false;
        AltEle = IUFindNumber(HorProp, "ALT");
        if (!AltEle)
            return false;

        currentAz  = AzEle->value;
        currentAlt = AltEle->value;
    }

    /* Could not find either properties! */
    if (EqProp == nullptr && HorProp == nullptr)
        return false;

    //targetAz = ScopeTarget->az().Degrees();
    targetAlt = ScopeTarget->altRefracted().Degrees();

    if (minAlt != -1 && maxAlt != -1)
    {
        if (targetAlt < minAlt || targetAlt > maxAlt)
        {
            KMessageBox::error(nullptr,
                               i18n("Requested altitude %1 is outside the specified altitude limit boundary (%2,%3).",
                                    QString::number(targetAlt, 'g', 3), QString::number(minAlt, 'g', 3),
                                    QString::number(maxAlt, 'g', 3)),
                               i18n("Telescope Motion"));
            return false;
        }
    }

    if (targetAlt < 0)
    {
        if (KMessageBox::warningContinueCancel(
                nullptr, i18n("Requested altitude is below the horizon. Are you sure you want to proceed?"),
                i18n("Telescope Motion"), KStandardGuiItem::cont(), KStandardGuiItem::cancel(),
                QString("telescope_coordintes_below_horizon_warning")) == KMessageBox::Cancel)
        {
            if (EqProp)
            {
                RAEle->value  = currentRA;
                DecEle->value = currentDEC;
            }
            if (HorProp)
            {
                AzEle->value  = currentAz;
                AltEle->value = currentAlt;
            }

            return false;
        }
    }

    if (EqProp)
    {
        RAEle->value  = ScopeTarget->ra().Hours();
        DecEle->value = ScopeTarget->dec().Degrees();
        clientManager->sendNewNumber(EqProp);

        qCDebug(KSTARS_INDI) << "ISD:Telescope: Sending coords RA " << RAEle->value << " DEC " << DecEle->value;

        RAEle->value  = currentRA;
        DecEle->value = currentDEC;
    }
    // Only send Horizontal Coord property if Equatorial is not available.
    else if (HorProp)
    {
        AzEle->value  = ScopeTarget->az().Degrees();
        AltEle->value = ScopeTarget->alt().Degrees();
        clientManager->sendNewNumber(HorProp);
        AzEle->value  = currentAz;
        AltEle->value = currentAlt;
    }

    double maxrad = 1000.0 / Options::zoomFactor();
    currentObject = KStarsData::Instance()->skyComposite()->objectNearest(ScopeTarget, maxrad);
    if (currentObject)
        emit newTarget(currentObject->name());

    return true;
}

bool Telescope::Slew(double ra, double dec)
{
    SkyPoint target;

    target.setRA(ra);
    target.setDec(dec);

    return Slew(&target);
}

bool Telescope::Slew(SkyPoint *ScopeTarget)
{
    ISwitchVectorProperty *motionSP = baseDevice->getSwitch("ON_COORD_SET");

    if (motionSP == nullptr)
        return false;

    ISwitch *slewSW = IUFindSwitch(motionSP, "TRACK");

    if (slewSW == nullptr)
        slewSW = IUFindSwitch(motionSP, "SLEW");

    if (slewSW == nullptr)
        return false;

    if (slewSW->s != ISS_ON)
    {
        IUResetSwitch(motionSP);
        slewSW->s = ISS_ON;
        clientManager->sendNewSwitch(motionSP);

        qCDebug(KSTARS_INDI) << "ISD:Telescope: " << slewSW->name;
    }

    return sendCoords(ScopeTarget);
}

bool Telescope::Sync(double ra, double dec)
{
    SkyPoint target;

    target.setRA(ra);
    target.setDec(dec);

    return Sync(&target);
}

bool Telescope::Sync(SkyPoint *ScopeTarget)
{
    ISwitchVectorProperty *motionSP = baseDevice->getSwitch("ON_COORD_SET");

    if (motionSP == nullptr)
        return false;

    ISwitch *syncSW = IUFindSwitch(motionSP, "SYNC");

    if (syncSW == nullptr)
        return false;

    if (syncSW->s != ISS_ON)
    {
        IUResetSwitch(motionSP);
        syncSW->s = ISS_ON;
        clientManager->sendNewSwitch(motionSP);

        qCDebug(KSTARS_INDI) << "ISD:Telescope: Syncing...";
    }

    return sendCoords(ScopeTarget);
}

bool Telescope::Abort()
{
    ISwitchVectorProperty *motionSP = baseDevice->getSwitch("TELESCOPE_ABORT_MOTION");

    if (motionSP == nullptr)
        return false;

    ISwitch *abortSW = IUFindSwitch(motionSP, "ABORT");

    if (abortSW == nullptr)
        return false;

    qCDebug(KSTARS_INDI) << "ISD:Telescope: Aborted." << endl;

    abortSW->s = ISS_ON;
    clientManager->sendNewSwitch(motionSP);

    return true;
}

bool Telescope::Park()
{
    ISwitchVectorProperty *parkSP = baseDevice->getSwitch("TELESCOPE_PARK");

    if (parkSP == nullptr)
        return false;

    ISwitch *parkSW = IUFindSwitch(parkSP, "PARK");

    if (parkSW == nullptr)
        return false;

    qCDebug(KSTARS_INDI) << "ISD:Telescope: Parking..." << endl;

    IUResetSwitch(parkSP);
    parkSW->s = ISS_ON;
    clientManager->sendNewSwitch(parkSP);

    return true;
}

bool Telescope::UnPark()
{
    ISwitchVectorProperty *parkSP = baseDevice->getSwitch("TELESCOPE_PARK");

    if (parkSP == nullptr)
        return false;

    ISwitch *parkSW = IUFindSwitch(parkSP, "UNPARK");

    if (parkSW == nullptr)
        return false;

    qCDebug(KSTARS_INDI) << "ISD:Telescope: UnParking..." << endl;

    IUResetSwitch(parkSP);
    parkSW->s = ISS_ON;
    clientManager->sendNewSwitch(parkSP);

    return true;
}

bool Telescope::getEqCoords(double *ra, double *dec)
{
    INumberVectorProperty *EqProp = nullptr;
    INumber *RAEle                = nullptr;
    INumber *DecEle               = nullptr;

    EqProp = baseDevice->getNumber("EQUATORIAL_EOD_COORD");
    if (EqProp == nullptr)
        return false;

    RAEle = IUFindNumber(EqProp, "RA");
    if (!RAEle)
        return false;
    DecEle = IUFindNumber(EqProp, "DEC");
    if (!DecEle)
        return false;

    *ra  = RAEle->value;
    *dec = DecEle->value;

    return true;
}

bool Telescope::MoveNS(TelescopeMotionNS dir, TelescopeMotionCommand cmd)
{
    ISwitchVectorProperty *motionSP = baseDevice->getSwitch("TELESCOPE_MOTION_NS");

    if (motionSP == nullptr)
        return false;

    ISwitch *motionNorth = IUFindSwitch(motionSP, "MOTION_NORTH");
    ISwitch *motionSouth = IUFindSwitch(motionSP, "MOTION_SOUTH");

    if (motionNorth == nullptr || motionSouth == nullptr)
        return false;

    // If same direction, return
    if (dir == MOTION_NORTH && motionNorth->s == ((cmd == MOTION_START) ? ISS_ON : ISS_OFF))
        return true;

    if (dir == MOTION_SOUTH && motionSouth->s == ((cmd == MOTION_START) ? ISS_ON : ISS_OFF))
        return true;

    IUResetSwitch(motionSP);

    if (cmd == MOTION_START)
    {
        if (dir == MOTION_NORTH)
            motionNorth->s = ISS_ON;
        else
            motionSouth->s = ISS_ON;
    }

    clientManager->sendNewSwitch(motionSP);

    return true;
}

bool Telescope::MoveWE(TelescopeMotionWE dir, TelescopeMotionCommand cmd)
{
    ISwitchVectorProperty *motionSP = baseDevice->getSwitch("TELESCOPE_MOTION_WE");

    if (motionSP == nullptr)
        return false;

    ISwitch *motionWest = IUFindSwitch(motionSP, "MOTION_WEST");
    ISwitch *motionEast = IUFindSwitch(motionSP, "MOTION_EAST");

    if (motionWest == nullptr || motionEast == nullptr)
        return false;

    // If same direction, return
    if (dir == MOTION_WEST && motionWest->s == ((cmd == MOTION_START) ? ISS_ON : ISS_OFF))
        return true;

    if (dir == MOTION_EAST && motionEast->s == ((cmd == MOTION_START) ? ISS_ON : ISS_OFF))
        return true;

    IUResetSwitch(motionSP);

    if (cmd == MOTION_START)
    {
        if (dir == MOTION_WEST)
            motionWest->s = ISS_ON;
        else
            motionEast->s = ISS_ON;
    }

    clientManager->sendNewSwitch(motionSP);

    return true;
}

bool Telescope::setSlewRate(int index)
{
    ISwitchVectorProperty *slewRateSP = baseDevice->getSwitch("TELESCOPE_SLEW_RATE");

    if (slewRateSP == nullptr)
        return false;

    if (index < 0 || index > slewRateSP->nsp)
        return false;

    IUResetSwitch(slewRateSP);

    slewRateSP->sp[index].s = ISS_ON;

    clientManager->sendNewSwitch(slewRateSP);

    return true;
}

void Telescope::setAltLimits(double minAltitude, double maxAltitude)
{
    minAlt = minAltitude;
    maxAlt = maxAltitude;
}

bool Telescope::setAlignmentModelEnabled(bool enable)
{
    bool wasExecuted                   = false;
    ISwitchVectorProperty *alignSwitch = nullptr;

    // For INDI Alignment Subsystem
    alignSwitch = baseDevice->getSwitch("ALIGNMENT_SUBSYSTEM_ACTIVE");
    if (alignSwitch)
    {
        alignSwitch->sp[0].s = enable ? ISS_ON : ISS_OFF;
        clientManager->sendNewSwitch(alignSwitch);
        wasExecuted = true;
    }

    // For EQMod Alignment --- Temporary until all drivers switch fully to INDI Alignment Subsystem
    alignSwitch = baseDevice->getSwitch("ALIGNMODE");
    if (alignSwitch)
    {
        IUResetSwitch(alignSwitch);
        // For now, always set alignment mode to NSTAR on enable.
        if (enable)
            alignSwitch->sp[2].s = ISS_ON;
        // Otherwise, set to NO ALIGN
        else
            alignSwitch->sp[0].s = ISS_ON;

        clientManager->sendNewSwitch(alignSwitch);
        wasExecuted = true;
    }

    return wasExecuted;
}

bool Telescope::clearAlignmentModel()
{
    bool wasExecuted = false;

    // Note: Should probably use INDI Alignment Subsystem Client API in the future?
    ISwitchVectorProperty *clearSwitch  = baseDevice->getSwitch("ALIGNMENT_POINTSET_ACTION");
    ISwitchVectorProperty *commitSwitch = baseDevice->getSwitch("ALIGNMENT_POINTSET_COMMIT");
    if (clearSwitch && commitSwitch)
    {
        IUResetSwitch(clearSwitch);
        // ALIGNMENT_POINTSET_ACTION.CLEAR
        clearSwitch->sp[4].s = ISS_ON;
        clientManager->sendNewSwitch(clearSwitch);
        commitSwitch->sp[0].s = ISS_ON;
        clientManager->sendNewSwitch(commitSwitch);
        wasExecuted = true;
    }

    // For EQMod Alignment --- Temporary until all drivers switch fully to INDI Alignment Subsystem
    clearSwitch = baseDevice->getSwitch("ALIGNLIST");
    if (clearSwitch)
    {
        // ALIGNLISTCLEAR
        IUResetSwitch(clearSwitch);
        clearSwitch->sp[1].s = ISS_ON;
        clientManager->sendNewSwitch(clearSwitch);
        wasExecuted = true;
    }

    return wasExecuted;
}

Telescope::TelescopeStatus Telescope::getStatus()
{
    INumberVectorProperty *EqProp = nullptr;

    EqProp = baseDevice->getNumber("EQUATORIAL_EOD_COORD");
    if (EqProp == nullptr)
        return MOUNT_ERROR;

    switch (EqProp->s)
    {
        case IPS_IDLE:
            if (inManualMotion)
                return MOUNT_MOVING;
            else if (isParked())
                return MOUNT_PARKED;
            else
                return MOUNT_IDLE;
            break;

        case IPS_OK:
            if (inManualMotion)
                return MOUNT_MOVING;
            else
                return MOUNT_TRACKING;
            break;

        case IPS_BUSY:
        {
            ISwitchVectorProperty *parkSP = baseDevice->getSwitch("TELESCOPE_PARK");
            if (parkSP && parkSP->s == IPS_BUSY)
                return MOUNT_PARKING;
            else
                return MOUNT_SLEWING;
        }
        break;

        case IPS_ALERT:
            return MOUNT_ERROR;
            break;
    }

    return MOUNT_ERROR;
}

const QString Telescope::getStatusString(Telescope::TelescopeStatus status)
{
    switch (status)
    {
        case ISD::Telescope::MOUNT_IDLE:
            return i18n("Idle");
            break;

        case ISD::Telescope::MOUNT_PARKED:
            return i18n("Parked");
            break;

        case ISD::Telescope::MOUNT_PARKING:
            return i18n("Parking");
            break;

        case ISD::Telescope::MOUNT_SLEWING:
            return i18n("Slewing");
            break;

        case ISD::Telescope::MOUNT_MOVING:
            return i18n("Moving %1", getManualMotionString());
            break;

        case ISD::Telescope::MOUNT_TRACKING:
            return i18n("Tracking");
            break;

        case ISD::Telescope::MOUNT_ERROR:
            return i18n("Error");
            break;
    }

    return i18n("Error");
}

QString Telescope::getManualMotionString() const
{
    ISwitchVectorProperty *movementSP = nullptr;
    QString NSMotion, WEMotion;

    movementSP = baseDevice->getSwitch("TELESCOPE_MOTION_NS");
    if (movementSP)
    {
        if (movementSP->sp[MOTION_NORTH].s == ISS_ON)
            NSMotion = 'N';
        else if (movementSP->sp[MOTION_SOUTH].s == ISS_ON)
            NSMotion = 'S';
    }

    movementSP = baseDevice->getSwitch("TELESCOPE_MOTION_WE");
    if (movementSP)
    {
        if (movementSP->sp[MOTION_WEST].s == ISS_ON)
            WEMotion = 'W';
        else if (movementSP->sp[MOTION_EAST].s == ISS_ON)
            WEMotion = 'E';
    }

    return QString("%1%2").arg(NSMotion, WEMotion);
}
}
