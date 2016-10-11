#include "bot.h"
#include "ai_shutdown_hooks_holder.h"

inline bool operator!=(const AiScriptWeaponDef &first, const AiScriptWeaponDef &second)
{
    return memcmp(&first, &second, sizeof(AiScriptWeaponDef)) != 0;
}

void Bot::UpdateScriptWeaponsStatus()
{
    int scriptWeaponsNum = GT_asGetScriptWeaponsNum(self->r.client);

    if ((int)scriptWeaponDefs.size() != scriptWeaponsNum)
    {
        scriptWeaponDefs.clear();
        scriptWeaponCooldown.clear();

        for (int weaponNum = 0; weaponNum < scriptWeaponsNum; ++weaponNum)
        {
            AiScriptWeaponDef weaponDef;
            if (GT_asGetScriptWeaponDef(self->r.client, weaponNum, &weaponDef))
            {
                scriptWeaponDefs.emplace_back(std::move(weaponDef));
                scriptWeaponCooldown.push_back(GT_asGetScriptWeaponCooldown(self->r.client, weaponNum));
            }
        }

        selectedWeapons.Invalidate();
        botBrain.ClearGoalAndPlan();
        return;
    }

    bool hasStatusChanged = false;
    for (int weaponNum = 0; weaponNum < scriptWeaponsNum; ++weaponNum)
    {
        AiScriptWeaponDef actualWeaponDef;
        // Try to retrieve the weapon def
        if (!GT_asGetScriptWeaponDef(self->r.client, weaponNum, &actualWeaponDef))
        {
            // If weapon def retrieval failed, treat the weapon as unavailable by setting a huge cooldown
            scriptWeaponCooldown[weaponNum] = std::numeric_limits<int>::max();
            hasStatusChanged = true;
            continue;
        }

        if (actualWeaponDef != scriptWeaponDefs[weaponNum])
        {
            scriptWeaponDefs[weaponNum] = actualWeaponDef;
            hasStatusChanged = true;
        }

        int cooldown = GT_asGetScriptWeaponCooldown(self->r.client, weaponNum);
        // A weapon became unavailable
        if (cooldown > scriptWeaponCooldown[weaponNum])
        {
            hasStatusChanged = true;
        }
        else
        {
            for (int thresholdMillis = 1000; thresholdMillis >= 0; thresholdMillis -= 500)
            {
                if (scriptWeaponCooldown[weaponNum] > thresholdMillis && cooldown <= thresholdMillis)
                    hasStatusChanged = true;
            }
        }
        scriptWeaponCooldown[weaponNum] = cooldown;
    }

    if (hasStatusChanged)
    {
        selectedWeapons.Invalidate();
        botBrain.ClearGoalAndPlan();
    }
}

bool Bot::FireWeapon(bool *didBuiltinAttack)
{
    if (!selectedEnemies.AreValid())
        return false;

    if (!selectedWeapons.AreValid())
        return false;

    const GenericFireDef *builtinFireDef = selectedWeapons.BuiltinFireDef();
    const GenericFireDef *scriptFireDef = selectedWeapons.ScriptFireDef();

    AimParams builtinWeaponAimParams;
    AimParams scriptWeaponAimParams;

    if (builtinFireDef)
        builtinFireTargetCache.AdjustAimParams(selectedEnemies, selectedWeapons, *builtinFireDef, &builtinWeaponAimParams);

    if (scriptFireDef)
        scriptFireTargetCache.AdjustAimParams(selectedEnemies, selectedWeapons, *scriptFireDef, &scriptWeaponAimParams);

    // Select a weapon that has a priority in adjusting view angles for it
    const GenericFireDef *primaryFireDef = nullptr;
    const GenericFireDef *secondaryFireDef = nullptr;
    AimParams *aimParams;
    if (selectedWeapons.PreferBuiltinWeapon())
    {
        aimParams = &builtinWeaponAimParams;
        primaryFireDef = builtinFireDef;
        if (scriptFireDef)
            secondaryFireDef = scriptFireDef;
    }
    else
    {
        aimParams = &scriptWeaponAimParams;
        primaryFireDef = scriptFireDef;
        if (builtinFireDef)
            secondaryFireDef = builtinFireDef;
    }

    // Always track enemy with a "crosshair" like a human does in each frame
    LookAtEnemy(aimParams->EffectiveAccuracy(Skill()), aimParams->fireOrigin, aimParams->fireTarget);

    // Attack only in Think() frames unless a continuousFire is required or the bot has hard skill
    if (ShouldSkipThinkFrame() && Skill() < 0.66f)
    {
        if (!primaryFireDef->IsContinuousFire())
        {
            if (!secondaryFireDef || !secondaryFireDef->IsContinuousFire())
                return false;
        }
    }

    bool didPrimaryAttack = false;
    bool didSecondaryAttack = false;

    if (CheckShot(*aimParams, selectedEnemies, *primaryFireDef))
        didPrimaryAttack = TryPressAttack(primaryFireDef, builtinFireDef, scriptFireDef, didBuiltinAttack);

    if (secondaryFireDef)
    {
        // Check whether view angles adjusted for the primary weapon are suitable for firing secondary weapon too
        if (CheckShot(*aimParams, selectedEnemies, *secondaryFireDef))
            didSecondaryAttack = TryPressAttack(secondaryFireDef, builtinFireDef, scriptFireDef, didBuiltinAttack);
    }

    return didPrimaryAttack || didSecondaryAttack;
}

void Bot::LookAtEnemy(float accuracy, const vec_t *fire_origin, vec_t *target)
{
    target[0] += (random() - 0.5f) * accuracy;
    target[1] += (random() - 0.5f) * accuracy;

    // TODO: Cancel pending turn?
    if (!pendingLookAtPointState.IsActive())
    {
        Vec3 lookAtVector(target);
        lookAtVector -= fire_origin;
        float angularSpeedMultiplier = 0.5f + 0.5f * Skill();
        bool extraPrecision = DistanceSquared(fire_origin, target) > 1100.0f * 1100.0f;
        ChangeAngle(lookAtVector, angularSpeedMultiplier, extraPrecision);
    }
}

bool Bot::TryPressAttack(const GenericFireDef *fireDef, const GenericFireDef *builtinFireDef,
                         const GenericFireDef *scriptFireDef, bool *didBuiltinAttack)
{
    if (fireDef == scriptFireDef)
        return GT_asFireScriptWeapon(self->r.client, fireDef->WeaponNum());

    auto weapState = self->r.client->ps.weaponState;
    *didBuiltinAttack = false;
    *didBuiltinAttack |= weapState == WEAPON_STATE_READY;
    *didBuiltinAttack |= weapState == WEAPON_STATE_REFIRE;
    *didBuiltinAttack |= weapState == WEAPON_STATE_REFIRESTRONG;

    return *didBuiltinAttack;
}

bool Bot::CheckShot(const AimParams &aimParams, const SelectedEnemies &selectedEnemies, const GenericFireDef &fireDef)
{
    // Do not shoot enemies that are far from "crosshair" except they are very close
    Vec3 newLookDir(0, 0, 0);
    AngleVectors(self->s.angles, newLookDir.Data(), nullptr, nullptr);


    Vec3 toTarget(aimParams.fireTarget);
    toTarget -= aimParams.fireOrigin;
    toTarget.NormalizeFast();
    float toTargetDotLookDir = toTarget.Dot(newLookDir);

    // 0 on zero range, 1 on distanceFactorBound range
    float directionDistanceFactor = 0.0001f;
    float squareDistanceToTarget = toTarget.SquaredLength();
    if (squareDistanceToTarget > 1)
    {
        float distance = 1.0f / Q_RSqrt(squareDistanceToTarget);
        directionDistanceFactor += BoundedFraction(distance, 450.0f);
    }

    // Precache this result, it is not just a value getter
    const auto aimType = fireDef.AimType();

    if (fireDef.IsContinuousFire())
    {
        if (toTargetDotLookDir < 0.8f * directionDistanceFactor)
            return false;
    }
    else if (aimType != AI_WEAPON_AIM_TYPE_DROP)
    {
        if (toTargetDotLookDir < 0.6f * directionDistanceFactor)
            return false;
    }
    else
    {
        if (toTargetDotLookDir < 0)
            return false;
    }

    // Do not shoot in enemies that are behind obstacles atm, bot may kill himself easily
    // We test directions factor first because it is cheaper to calculate

    trace_t tr;
    if (aimType != AI_WEAPON_AIM_TYPE_DROP)
    {
        Vec3 traceEnd(newLookDir);
        traceEnd *= 999999.0f;
        traceEnd += aimParams.fireOrigin;
        G_Trace(&tr, const_cast<float*>(aimParams.fireOrigin), nullptr, nullptr, traceEnd.Data(), self, MASK_AISOLID);
        if (tr.fraction == 1.0f)
            return true;
    }
    else
    {
        // For drop aim type weapons (a gravity is applied to a projectile) split projectile trajectory in segments
        vec3_t segmentStart;
        vec3_t segmentEnd;
        VectorCopy(aimParams.fireOrigin, segmentEnd);

        Vec3 projectileVelocity(newLookDir);
        projectileVelocity *= fireDef.ProjectileSpeed();

        const int numSegments = (int)(2 + 4 * Skill());
        // Predict for 1 second
        const float timeStep = 1.0f / numSegments;
        const float halfGravity = 0.5f * level.gravity;
        const float *fireOrigin = aimParams.fireOrigin;

        float currTime = timeStep;
        for (int i = 0; i < numSegments; ++i)
        {
            VectorCopy(segmentEnd, segmentStart);
            segmentEnd[0] = fireOrigin[0] + projectileVelocity.X() * currTime;
            segmentEnd[1] = fireOrigin[1] + projectileVelocity.Y() * currTime;
            segmentEnd[2] = fireOrigin[2] + projectileVelocity.Z() * currTime - halfGravity * currTime * currTime;

            G_Trace(&tr, segmentStart, nullptr, nullptr, segmentEnd, self, MASK_AISOLID);
            if (tr.fraction != 1.0f)
                break;

            currTime += timeStep;
        }
        // If hit point has not been found for predicted for 1 second trajectory
        if (tr.fraction == 1.0f)
        {
            // Check a trace from the last segment end to an infinite point
            VectorCopy(segmentEnd, segmentStart);
            currTime = 999.0f;
            segmentEnd[0] = fireOrigin[0] + projectileVelocity.X() * currTime;
            segmentEnd[1] = fireOrigin[1] + projectileVelocity.Y() * currTime;
            segmentEnd[2] = fireOrigin[2] + projectileVelocity.Z() * currTime - halfGravity * currTime * currTime;
            G_Trace(&tr, segmentStart, nullptr, nullptr, segmentEnd, self, MASK_AISOLID);
            if (tr.fraction == 1.0f)
                return true;
        }
    }

    if (game.edicts[tr.ent].s.team == self->s.team && GS_TeamBasedGametype())
        return false;

    float hitToTargetDist = DistanceFast(selectedEnemies.LastSeenOrigin().Data(), tr.endpos);
    float hitToBotDist = DistanceFast(self->s.origin, tr.endpos);
    float proximityDistanceFactor = BoundedFraction(hitToBotDist, 2000.0f);
    float hitToTargetMissThreshold = 30.0f + 300.0f * proximityDistanceFactor;

    if (hitToBotDist < hitToTargetDist && !fireDef.IsContinuousFire())
        return false;

    if (aimType == AI_WEAPON_AIM_TYPE_PREDICTION_EXPLOSIVE)
        return hitToTargetDist < std::max(hitToTargetMissThreshold, 0.85f * fireDef.SplashRadius());

    // Trajectory prediction is not accurate, also this adds some randomization in grenade spamming.
    if (aimType == AI_WEAPON_AIM_TYPE_DROP)
    {
        // Allow shooting grenades in vertical walls
        if (DotProduct(tr.plane.normal, &axis_identity[AXIS_UP]) < -0.1f)
            return false;

        return hitToTargetDist < std::max(hitToTargetMissThreshold, 1.15f * fireDef.SplashRadius());
    }

    // For one-shot instant-hit weapons each shot is important, so check against a player bounding box
    Vec3 absMins(aimParams.fireTarget);
    Vec3 absMaxs(aimParams.fireTarget);
    absMins += playerbox_stand_mins;
    absMaxs += playerbox_stand_maxs;
    float factor = 0.33f;
    // Extra hack for EB/IG, otherwise they miss too lot due to premature firing
    if (fireDef.IsBuiltin())
    {
        if (fireDef.WeaponNum() == WEAP_ELECTROBOLT || fireDef.WeaponNum() == WEAP_INSTAGUN)
            factor *= std::max(0.0f, 0.66f - Skill());
    }
    return BoundsAndSphereIntersect(absMins.Data(), absMaxs.Data(), tr.endpos, 1.0f + factor * hitToTargetMissThreshold);
}


