/** @file move.c
 *
 * Source for handling movement on the MUCK, such as moving a player from
 * room to room.
 *
 * This file is part of Fuzzball MUCK.  Please see LICENSE.md for details.
 */

#include <stdio.h>
#include <string.h>

#include "config.h"

#include "boolexp.h"
#include "commands.h"
#include "db.h"
#include "edit.h"
#include "fbstrings.h"
#include "fbtime.h"
#include "game.h"
#include "interface.h"
#include "interp.h"
#include "log.h"
#include "match.h"
#include "move.h"
#include "predicates.h"
#include "props.h"
#include "timequeue.h"
#include "tune.h"
#include "fbmath.h"

static int donelook = 0;

static void
maybe_dropto(int descr, dbref loc, dbref dropto)
{
    dbref thing;

    if (loc == dropto)
	return;			/* bizarre special case */

    /* check for players */
    DOLIST(thing, CONTENTS(loc)) {
	/* Make zombies act like players for dropto processing */
	if (Typeof(thing) == TYPE_PLAYER ||
	    (Typeof(thing) == TYPE_THING && FLAGS(thing) & ZOMBIE))
	    return;
    }

    /* no players, send everything to the dropto */
    send_contents(descr, loc, dropto);
}

void
enter_room(int descr, dbref player, dbref loc, dbref exit)
{
    dbref old;
    dbref dropto;
    char buf[BUFFER_LEN];

    /* check for room == HOME */
    if (loc == HOME)
	loc = PLAYER_HOME(player);	/* home */

    /* get old location */
    old = LOCATION(player);

    if (parent_loop_check(player, loc)) {
	switch (Typeof(player)) {
	case TYPE_PLAYER:
	    loc = PLAYER_HOME(player);
	    break;
	case TYPE_THING:
	    loc = THING_HOME(player);
	    if (parent_loop_check(player, loc)) {
		loc = PLAYER_HOME(OWNER(player));
		if (parent_loop_check(player, loc))
		    loc = (dbref) tp_player_start;
	    }
	    break;
	case TYPE_ROOM:
	    loc = GLOBAL_ENVIRONMENT;
	    break;
	case TYPE_PROGRAM:
	    loc = OWNER(player);
	    break;
	}
    }

    /* check for self-loop */
    /* self-loops don't do move or other player notification */
    /* but you still get autolook and penny check */
    if (loc != old) {

	/* go there */
	moveto(player, loc);

	if (old != NOTHING) {
	    propqueue(descr, player, old, exit, player, NOTHING, DEPART_PROPQUEUE, "Depart", 1, 1);
	    envpropqueue(descr, player, old, exit, old, NOTHING, DEPART_PROPQUEUE, "Depart", 1, 1);

	    propqueue(descr, player, old, exit, player, NOTHING, ODEPART_PROPQUEUE, "Odepart", 1, 0);
	    envpropqueue(descr, player, old, exit, old, NOTHING, ODEPART_PROPQUEUE, "Odepart", 1, 0);

	    /* notify others unless DARK */
	    if (!tp_quiet_moves &&
                !Dark(old) && !Dark(player) &&
		((Typeof(player) != TYPE_THING) ||
		 ((Typeof(player) == TYPE_THING) && (FLAGS(player) & (ZOMBIE | VEHICLE))))
		&& (Typeof(exit) != TYPE_EXIT || !Dark(exit))) {
		snprintf(buf, sizeof(buf), "%s has left.", NAME(player));
		notify_except(CONTENTS(old), player, buf, player);
	    }
	}

	/* if old location has STICKY dropto, send stuff through it */
	if (old != NOTHING && Typeof(old) == TYPE_ROOM
	    && (dropto = DBFETCH(old)->sp.room.dropto) != NOTHING && (FLAGS(old) & STICKY)) {
	    maybe_dropto(descr, old, dropto);
	}

	/* tell other folks in new location if not DARK */
	if (!tp_quiet_moves &&
            !Dark(loc) && !Dark(player) &&
	    ((Typeof(player) != TYPE_THING) ||
	     ((Typeof(player) == TYPE_THING) && (FLAGS(player) & (ZOMBIE | VEHICLE))))
	    && (Typeof(exit) != TYPE_EXIT || !Dark(exit))) {
	    snprintf(buf, sizeof(buf), "%s has arrived.", NAME(player));
	    notify_except(CONTENTS(loc), player, buf, player);
	}
    }
    /* autolook */
    if ((Typeof(player) != TYPE_THING) ||
	((Typeof(player) == TYPE_THING) && (FLAGS(player) & (ZOMBIE | VEHICLE)))) {
	if (donelook < 8) {
	    donelook++;
	    if (can_move(descr, player, tp_autolook_cmd, 1)) {
                do_move(descr, player, tp_autolook_cmd, 1);
	    } else {
                look_room(descr, player, LOCATION(player));
	    }
	    donelook--;
	} else {
	    notify(player, "Look aborted because of look action loop.");
	}
    }

    if (tp_penny_rate != 0) {
	/* check for pennies */
	if (!controls(player, loc)
	    && GETVALUE(OWNER(player)) <= tp_max_pennies && RANDOM() % tp_penny_rate == 0) {
	    notifyf(player, "You found one %s!", tp_penny);
	    SETVALUE(OWNER(player), GETVALUE(OWNER(player)) + 1);
	    DBDIRTY(OWNER(player));
	}
    }

    if (loc != old) {
	envpropqueue(descr, player, loc, exit, player, NOTHING, ARRIVE_PROPQUEUE, "Arrive", 1, 1);
	envpropqueue(descr, player, loc, exit, player, NOTHING, OARRIVE_PROPQUEUE, "Oarrive", 1, 0);
    }
}

void
moveto(dbref what, dbref where)
{
    dbref loc;

    /* do NOT move garbage */
    if (what != NOTHING && Typeof(what) == TYPE_GARBAGE) {
	return;
    }

    /* remove what from old loc */
    if ((loc = LOCATION(what)) != NOTHING) {
	DBSTORE(loc, contents, remove_first(CONTENTS(loc), what));
    }
    /* test for special cases */
    switch (where) {
    case NOTHING:
	DBSTORE(what, location, NOTHING);
	return;			/* NOTHING doesn't have contents */
    case HOME:
	switch (Typeof(what)) {
	case TYPE_PLAYER:
	    where = PLAYER_HOME(what);
	    break;
	case TYPE_THING:
	    where = THING_HOME(what);
	    if (parent_loop_check(what, where)) {
		where = PLAYER_HOME(OWNER(what));
		if (parent_loop_check(what, where))
		    where = (dbref) tp_player_start;
	    }
	    break;
	case TYPE_ROOM:
	    where = GLOBAL_ENVIRONMENT;
	    break;
	case TYPE_PROGRAM:
	    where = OWNER(what);
	    break;
	}
	break;
    default:
	if (parent_loop_check(what, where)) {
	    switch (Typeof(what)) {
	    case TYPE_PLAYER:
		where = PLAYER_HOME(what);
		break;
	    case TYPE_THING:
		where = THING_HOME(what);
		if (parent_loop_check(what, where)) {
		    where = PLAYER_HOME(OWNER(what));
		    if (parent_loop_check(what, where))
			where = (dbref) tp_player_start;
		}
		break;
	    case TYPE_ROOM:
		where = GLOBAL_ENVIRONMENT;
		break;
	    case TYPE_PROGRAM:
		where = OWNER(what);
		break;
	    }
	}
    }

    /* now put what in where */
    PUSH(what, CONTENTS(where));
    DBDIRTY(where);
    DBSTORE(what, location, where);
}

void
send_contents(int descr, dbref loc, dbref dest)
{
    dbref first;
    dbref rest;
    dbref where;

    first = CONTENTS(loc);
    DBSTORE(loc, contents, NOTHING);

    /* blast locations of everything in list */
    DOLIST(rest, first) {
	DBSTORE(rest, location, NOTHING);
    }

    while (first != NOTHING) {
	rest = NEXTOBJ(first);
	if ((Typeof(first) != TYPE_THING)
	    && (Typeof(first) != TYPE_PROGRAM)) {
	    moveto(first, loc);
	} else {
	    where = FLAGS(first) & STICKY ? HOME : dest;
	    if (tp_secure_thing_movement && (Typeof(first) == TYPE_THING)) {
		enter_room(descr, first,
			   parent_loop_check(first, where) ? loc : where, LOCATION(first));
	    } else {
		moveto(first, parent_loop_check(first, where) ? loc : where);
	    }
	}
	first = rest;
    }

    DBSTORE(loc, contents, reverse(CONTENTS(loc)));
}

void
send_home(int descr, dbref thing, int puppethome)
{
    switch (Typeof(thing)) {
    case TYPE_PLAYER:
	/* send his possessions home first! */
	/* that way he sees them when he arrives */
	send_contents(descr, thing, HOME);
	enter_room(descr, thing, PLAYER_HOME(thing), LOCATION(thing));
	break;
    case TYPE_THING:
	if (puppethome)
	    send_contents(descr, thing, HOME);
	if (tp_secure_thing_movement || (FLAGS(thing) & (ZOMBIE | LISTENER))) {
	    enter_room(descr, thing, PLAYER_HOME(thing), LOCATION(thing));
	    break;
	}
	moveto(thing, HOME);	/* home */
	break;
    case TYPE_PROGRAM:
	moveto(thing, OWNER(thing));
	break;
    default:
	/* no effect */
	break;
    }
    return;
}

/*
 * trigger()
 *
 * This procedure triggers a series of actions, or meta-actions
 * which are contained in the 'dest' field of the exit.
 * Locks other than the first one are over-ridden.
 *
 * `player' is the player who triggered the exit
 * `exit' is the exit triggered
 * `pflag' is a flag which indicates whether player and room exits
 * are to be used (non-zero) or ignored (zero).  Note that
 * player/room destinations triggered via a meta-link are
 * ignored.
 *
 */

static void
trigger(int descr, dbref player, dbref exit, int pflag)
{
    dbref dest;
    int sobjact;		/* sticky object action flag, sends home

				 * source obj */
    int succ;
    struct frame *tmpfr;

    sobjact = 0;
    succ = 0;

    for (int i = 0; i < DBFETCH(exit)->sp.exit.ndest; i++) {
	dest = (DBFETCH(exit)->sp.exit.dest)[i];
	if (dest == HOME) {
	    dest = PLAYER_HOME(player);

	    /* fix #1112946 temporarily -- premchai21 */
	    if (Typeof(dest) == TYPE_THING) {
		notify(player, "That would be an undefined operation.");
		continue;
	    }
	}
	if (dest == NIL) {
	    succ = 1;
	} else {
	    switch (Typeof(dest)) {
	    case TYPE_ROOM:
		if (pflag) {
		    if (parent_loop_check(player, dest)) {
			notify(player, "That would cause a paradox.");
			break;
		    }
		    if (!Wizard(OWNER(player)) && Typeof(player) == TYPE_THING
			&& (FLAGS(dest) & ZOMBIE)) {
			notify(player, "You can't go that way.");
			break;
		    }
		    if ((FLAGS(player) & VEHICLE) && ((FLAGS(dest) | FLAGS(exit)) & VEHICLE)) {
			notify(player, "You can't go that way.");
			break;
		    }
		    if (ISGUEST(player) && ((FLAGS(dest) | FLAGS(exit)) & GUEST)) {
			notify(player, "You can't go that way.");
			break;
		    }
		    if (GETDROP(exit))
			exec_or_notify_prop(descr, player, exit, MESGPROP_DROP, "(@Drop)");
		    if (GETODROP(exit) && !Dark(player)) {
			parse_oprop(descr, player, dest, exit, MESGPROP_ODROP,
				    NAME(player), "(@Odrop)");
		    }
		    enter_room(descr, player, dest, exit);
		    succ = 1;
		}
		break;
	    case TYPE_THING:
		if (dest == LOCATION(exit) && (FLAGS(dest) & VEHICLE)) {
		    if (pflag) {
			if (parent_loop_check(player, dest)) {
			    notify(player, "That would cause a paradox.");
			    break;
			}
			if (GETDROP(exit))
			    exec_or_notify_prop(descr, player, exit, MESGPROP_DROP, "(@Drop)");
			if (GETODROP(exit) && !Dark(player)) {
			    parse_oprop(descr, player, dest, exit, MESGPROP_ODROP,
					NAME(player), "(@Odrop)");
			}
			enter_room(descr, player, dest, exit);
			succ = 1;
		    }
		} else {
		    if (Typeof(LOCATION(exit)) == TYPE_THING) {
			if (parent_loop_check(dest, LOCATION(LOCATION(exit)))) {
			    notify(player, "That would cause a paradox.");
			    break;
			}
			if (tp_secure_thing_movement) {
			    enter_room(descr, dest, LOCATION(LOCATION(exit)), exit);
			} else {
			    moveto(dest, LOCATION(LOCATION(exit)));
			}
			if (!(FLAGS(exit) & STICKY)) {
			    /* send home source object */
			    sobjact = 1;
			}
		    } else {
			if (parent_loop_check(dest, LOCATION(exit))) {
			    notify(player, "That would cause a paradox.");
			    break;
			}
			if (tp_secure_thing_movement) {
			    enter_room(descr, dest, LOCATION(exit), exit);
			} else {
			    moveto(dest, LOCATION(exit));
			}
		    }
		    if (GETSUCC(exit))
			succ = 1;
		}
		break;
	    case TYPE_EXIT:	/* It's a meta-link(tm)! */
		ts_useobject(dest);
		trigger(descr, player, (DBFETCH(exit)->sp.exit.dest)[i], 0);
		if (GETSUCC(exit))
		    succ = 1;
		break;
	    case TYPE_PLAYER:
		if (pflag && LOCATION(dest) != NOTHING) {
		    if (parent_loop_check(player, dest)) {
			notify(player, "That would cause a paradox.");
			break;
		    }
		    succ = 1;
		    if (FLAGS(dest) & JUMP_OK) {
			if (GETDROP(exit)) {
			    exec_or_notify_prop(descr, player, exit, MESGPROP_DROP, "(@Drop)");
			}
			if (GETODROP(exit) && !Dark(player)) {
			    parse_oprop(descr, player, LOCATION(dest), exit,
					MESGPROP_ODROP, NAME(player), "(@Odrop)");
			}
			enter_room(descr, player, LOCATION(dest), exit);
		    } else {
			notify(player, "That player does not wish to be disturbed.");
		    }
		}
		break;
	    case TYPE_PROGRAM:
	        if (ISGUEST(player) && ((FLAGS(dest) | FLAGS(exit)) & GUEST)) {
		    notify(player, "You can't go that way.");
		    break;
	        }
		tmpfr = interp(descr, player, LOCATION(player), dest, exit,
			       FOREGROUND, STD_REGUID, 0);
		if (tmpfr) {
		    interp_loop(player, dest, tmpfr, 0);
		}
		return;
	    }
	}
    }
    if (sobjact)
	send_home(descr, LOCATION(exit), 0);
    if (!succ && pflag)
	notify(player, "Done.");
}

/**
 * Implementation of the move command
 *
 * The move command is kind of like the 'start' command in Windows or the
 * 'open' command in Mac ... it makes the player go through an exit, which
 * can be any exit that matches, even a MUF program.
 *
 * lev should usually be 0 - @see match_all_exits
 *
 * @param descr the descriptor of the moving player
 * @param player the player moving
 * @param direction the exit to search for
 * @param lev the match level, passed to match_all_exits via match_data md
 */
void
do_move(int descr, dbref player, const char *direction, int lev)
{
    dbref exit;
    dbref loc;
    char buf[BUFFER_LEN];
    struct match_data md;

    if (tp_enable_home && !strcasecmp(direction, "home")) {
        /* send him home */
        /* but steal all his possessions */
        if ((loc = LOCATION(player)) != NOTHING) {
            /* tell everybody else */
            snprintf(buf, sizeof(buf), "%s goes home.", NAME(player));
            notify_except(CONTENTS(loc), player, buf, player);
        }

        /* give the player the messages */
        notify(player, "There's no place like home...");
        notify(player, "There's no place like home...");
        notify(player, "There's no place like home...");
        notify(player, "You wake up back home, without your possessions.");
        send_home(descr, player, 1);
    } else {
        /* find the exit */
        init_match_check_keys(descr, player, direction, TYPE_EXIT, &md);
        md.match_level = lev;
        match_all_exits(&md);

        if ((exit = noisy_match_result(&md)) == NOTHING) {
            return;
        }

        ts_useobject(exit);

        if (can_doit(descr, player, exit, "You can't go that way.")) {
            trigger(descr, player, exit, 1);
        }
    }
}

/**
 * Implementation of the leave command
 *
 * This is for a player to leave a vehicle.  Does all the flag checking
 * to make sure the player is able to leave.
 *
 * @param descr the descriptor of the leaving player
 * @param player the player leaving
 */
void
do_leave(int descr, dbref player)
{
    dbref loc, dest;

    loc = LOCATION(player);

    if (Typeof(loc) == TYPE_ROOM) {
        notify(player, "You can't go that way.");
        return;
    }

    if (!(FLAGS(loc) & VEHICLE)) {
        notify(player, "You can only exit vehicles.");
        return;
    }

    dest = LOCATION(loc);

    if (Typeof(dest) != TYPE_ROOM && Typeof(dest) != TYPE_THING) {
        notify(player, "You can't exit a vehicle inside of a player.");
        return;
    }

    if (parent_loop_check(player, dest)) {
        notify(player, "You can't go that way.");
        return;
    }

    notify(player, "You exit the vehicle.");
    enter_room(descr, player, dest, loc);
}

/**
 * Implementation of get command
 *
 * This implements the 'get' command, including the ability to get
 * objects from a container.
 *
 * This does all the necessary permission checking.  If 'obj' is
 * provided, we will try to get 'what' out of container 'obj'.
 *
 * If 'obj' is empty, it will get it from the current room or get
 * it by DBREF if you're a wizard.
 *
 * A lot of the permission and messaging handling is delegated to common
 * calls could_doit and can_doit.
 *
 * @see could_doit
 * @see can_doit
 *
 * @param descr the descriptor of the person getting
 * @param player the person getting
 * @param what what to pick up
 * @param obj the container, if applicable, or ""
 */
void
do_get(int descr, dbref player, const char *what, const char *obj)
{
    dbref thing, cont;
    int cando;
    struct match_data md;

    init_match_check_keys(descr, player, what, TYPE_THING, &md);
    match_neighbor(&md);
    match_possession(&md);

    if (Wizard(OWNER(player)))
        match_absolute(&md);    /* the wizard has long fingers */

    if ((thing = noisy_match_result(&md)) == NOTHING) {
        return;
    }

    cont = thing;

    /* Do we have a container? */
    if (obj && *obj) {
        init_match_check_keys(descr, player, obj, TYPE_THING, &md);
        match_rmatch(cont, &md);

        if (Wizard(OWNER(player)))
            match_absolute(&md);    /* the wizard has long fingers */

        if ((thing = noisy_match_result(&md)) == NOTHING) {
            return;
        }

        if (!test_lock_false_default(descr, player, cont, MESGPROP_CONLOCK)) {
            notify(player, "You can't open that container.");
            return;
        }
    }

    /* Do some checking around zombies to make sure they aren't
     * doing anything fishy (picking up something not in a room)
     *
     * I believe this means zombies can't take things from other
     * people's containers.
     */
    if (Typeof(player) != TYPE_PLAYER) {
        if (LOCATION(thing) != NOTHING && Typeof(LOCATION(thing)) != TYPE_ROOM) {
            if (OWNER(player) != OWNER(thing)) {
                notify(player, "Zombies aren't allowed to be thieves!");
                return;
            }
        }
    }

    if (LOCATION(thing) == player) {
        notify(player, "You already have that!");
        return;
    }

    if (Typeof(cont) == TYPE_PLAYER) {
        notify(player, "You can't steal stuff from players.");
        return;
    }

    if (parent_loop_check(thing, player)) {
        notify(player, "You can't pick yourself up by your bootstraps!");
        return;
    }

    switch (Typeof(thing)) {
        case TYPE_THING:
            ts_useobject(thing);
        case TYPE_PROGRAM:
            if (obj && *obj) {
                cando = could_doit(descr, player, thing);

                if (!cando)
                    notify(player, "You can't get that.");
                } else {
                    cando = can_doit(descr, player, thing,
                                     "You can't pick that up.");
                }

                if (cando) {
                    if (tp_secure_thing_movement && (Typeof(thing) == TYPE_THING)) {
                        enter_room(descr, thing, player, LOCATION(thing));
                    } else {
                        moveto(thing, player);
                    }

                    notify(player, "Taken.");
                }

                break;
        default:
            notify(player, "You can't take that!");
            break;
    }
}

/**
 * Implementation of drop, put, and throw commands
 *
 * All three commands are completley identical under the hood.  The
 * only difference is their help files.
 *
 * The logic here is somewhat complicated as a result.  If just
 * 'name' is provided, this command acts like drop and seeks to
 * drop the given object in the caller's posession into the room.
 *
 * If 'obj' is also provided, then the 'throw'/'put' behavior is
 * in play.
 *
 * For the put/throw logic, if the target object is not a room, it must
 * have a container lock.  The lock is "@conlock" or "_/clk".  If there is
 * no lock it will default to false.
 *
 * This handles all the nuances of dropping things, such as drop/odrop
 * messages and room STICKY flags and drop-to's.
 *
 * @param descr the descriptor of the dropping player
 * @param player the dropping player
 * @param name the name of the object to drop
 * @param obj "" if dropping to current room, target of throw/put otherwise
 */
void
do_drop(int descr, dbref player, const char *name, const char *obj)
{
    dbref loc, cont;
    dbref thing;
    char buf[BUFFER_LEN];
    struct match_data md;

    loc = LOCATION(player);

    init_match(descr, player, name, NOTYPE, &md);
    match_possession(&md);

    if ((thing = noisy_match_result(&md)) == NOTHING)
        return;

    cont = loc;

    if (obj && *obj) {
        init_match(descr, player, obj, NOTYPE, &md);
        match_possession(&md);
        match_neighbor(&md);

        if (Wizard(OWNER(player)))
            match_absolute(&md);    /* the wizard has long fingers */

            if ((cont = noisy_match_result(&md)) == NOTHING) {
                return;
            }
    }

    switch (Typeof(thing)) {
        case TYPE_THING:
            ts_useobject(thing);
        case TYPE_PROGRAM:
            if (LOCATION(thing) != player) {
                /* Shouldn't ever happen. */
                notify(player, "You can't drop that.");
                break;
            }

            if (Typeof(cont) != TYPE_ROOM && Typeof(cont) != TYPE_PLAYER &&
                Typeof(cont) != TYPE_THING) {
                notify(player, "You can't put anything in that.");
                break;
            }

            if (Typeof(cont) != TYPE_ROOM &&
                !test_lock_false_default(descr, player, cont, MESGPROP_CONLOCK)) {
                notify(player, "You don't have permission to put something in that.");
                break;
            }

            if (parent_loop_check(thing, cont)) {
                notify(player, "You can't put something inside of itself.");
                break;
            }

            if (Typeof(cont) == TYPE_ROOM && (FLAGS(thing) & STICKY) &&
                Typeof(thing) == TYPE_THING) {
                send_home(descr, thing, 0);
            } else {
                int immediate_dropto = (Typeof(cont) == TYPE_ROOM &&
                                        DBFETCH(cont)->sp.room.dropto != NOTHING
                                        && !(FLAGS(cont) & STICKY));

                if (tp_secure_thing_movement && (Typeof(thing) == TYPE_THING)) {
                    enter_room(descr, thing,
                               immediate_dropto ? DBFETCH(cont)->sp.room.dropto : cont, player);
                } else {
                    moveto(thing, immediate_dropto ? DBFETCH(cont)->sp.room.dropto : cont);
                }
            }

            if (Typeof(cont) == TYPE_THING) {
                notify(player, "Put away.");
                return;
            } else if (Typeof(cont) == TYPE_PLAYER) {
                notifyf(cont, "%s hands you %s", NAME(player), NAME(thing));
                notifyf(player, "You hand %s to %s", NAME(thing), NAME(cont));
                return;
            }

            if (GETDROP(thing))
                exec_or_notify_prop(descr, player, thing, MESGPROP_DROP, "(@Drop)");
            else
                notify(player, "Dropped.");

            if (GETDROP(loc))
                exec_or_notify_prop(descr, player, loc, MESGPROP_DROP, "(@Drop)");

            if (GETODROP(thing)) {
                parse_oprop(descr, player, loc, thing, MESGPROP_ODROP, NAME(player), "(@Odrop)");
            } else {
                snprintf(buf, sizeof(buf), "%s drops %s.", NAME(player), NAME(thing));
                notify_except(CONTENTS(loc), player, buf, player);
            }

            if (GETODROP(loc)) {
                parse_oprop(descr, player, loc, loc, MESGPROP_ODROP, NAME(thing), "(@Odrop)");
            }

            break;
        default:
            notify(player, "You can't drop that.");
            break;
    }
}

void
recycle(int descr, dbref player, dbref thing)
{
    static int depth = 0;
    dbref first;
    dbref rest;
    char buf[2048];
    int looplimit;

    depth++;
    if (force_level) {
	if (objnode_find(forcelist, thing)) {
	    log_status("SANITYCHECK: Was about to recycle FORCEing object #%d!", thing);
	    notify(player, "ERROR: Cannot recycle an object FORCEing you!");
	    return;
	}
	if ((Typeof(thing) == TYPE_PROGRAM) && (PROGRAM_INSTANCES(thing) != 0)) {
	    log_status("SANITYCHECK: Trying to recycle a running program (#%d) from FORCE!",
		       thing);
	    notify(player, "ERROR: Cannot recycle a running program from FORCE.");
	    return;
	}
    }
    /* dequeue any MUF or MPI events for the given object */
    dequeue_prog(thing, 0);
    switch (Typeof(thing)) {
    case TYPE_ROOM:
	if (!Wizard(OWNER(thing)))
	    SETVALUE(OWNER(thing), GETVALUE(OWNER(thing)) + tp_room_cost);
	DBDIRTY(OWNER(thing));
	for (first = EXITS(thing); first != NOTHING; first = rest) {
	    rest = NEXTOBJ(first);
	    if (LOCATION(first) == NOTHING || LOCATION(first) == thing)
		recycle(descr, player, first);
	}
	notify_except(CONTENTS(thing), NOTHING, "You feel a wrenching sensation...", player);
	break;
    case TYPE_THING:
	if (!Wizard(OWNER(thing)))
	    SETVALUE(OWNER(thing), GETVALUE(OWNER(thing)) + MAX(0,GETVALUE(thing)));
	DBDIRTY(OWNER(thing));
	for (first = EXITS(thing); first != NOTHING; first = rest) {
	    rest = NEXTOBJ(first);
	    if (LOCATION(first) == NOTHING || LOCATION(first) == thing)
		recycle(descr, player, first);
	}
	break;
    case TYPE_EXIT:
	if (!Wizard(OWNER(thing)))
	    SETVALUE(OWNER(thing), GETVALUE(OWNER(thing)) + tp_exit_cost);
	if (!Wizard(OWNER(thing)))
	    if (DBFETCH(thing)->sp.exit.ndest != 0)
		SETVALUE(OWNER(thing), GETVALUE(OWNER(thing)) + tp_link_cost);
	DBDIRTY(OWNER(thing));
	break;
    case TYPE_PROGRAM:
	snprintf(buf, sizeof(buf), "muf/%d.m", (int) thing);
	unlink(buf);
	break;
    }

    for (rest = 0; rest < db_top; rest++) {
	switch (Typeof(rest)) {
	case TYPE_ROOM:
	    if (DBFETCH(rest)->sp.room.dropto == thing) {
		DBFETCH(rest)->sp.room.dropto = NOTHING;
		DBDIRTY(rest);
	    }
	    if (EXITS(rest) == thing) {
		EXITS(rest) = NEXTOBJ(thing);
		DBDIRTY(rest);
	    }
	    if (OWNER(rest) == thing) {
		OWNER(rest) = GOD;
		DBDIRTY(rest);
	    }
	    break;
	case TYPE_THING:
	    if (THING_HOME(rest) == thing) {
		dbref loc;

		if (PLAYER_HOME(OWNER(rest)) == thing)
		    PLAYER_SET_HOME(OWNER(rest), tp_player_start);
		loc = PLAYER_HOME(OWNER(rest));
		if (parent_loop_check(rest, loc)) {
		    loc = OWNER(rest);
		    if (parent_loop_check(rest, loc)) {
			loc = (dbref) 0;
		    }
		}
		THING_SET_HOME(rest, loc);
		DBDIRTY(rest);
	    }
	    if (EXITS(rest) == thing) {
		EXITS(rest) = NEXTOBJ(thing);
		DBDIRTY(rest);
	    }
	    if (OWNER(rest) == thing) {
		OWNER(rest) = GOD;
		DBDIRTY(rest);
	    }
	    break;
	case TYPE_EXIT:
	    {
		int j;

		for (int i = j = 0; i < DBFETCH(rest)->sp.exit.ndest; i++) {
		    if ((DBFETCH(rest)->sp.exit.dest)[i] != thing)
			(DBFETCH(rest)->sp.exit.dest)[j++] = (DBFETCH(rest)->sp.exit.dest)[i];
		}
		if (j < DBFETCH(rest)->sp.exit.ndest) {
		    SETVALUE(OWNER(rest), GETVALUE(OWNER(rest)) + tp_link_cost);
		    DBDIRTY(OWNER(rest));
		    DBFETCH(rest)->sp.exit.ndest = j;
		    DBDIRTY(rest);
		}
	    }
	    if (OWNER(rest) == thing) {
		OWNER(rest) = GOD;
		DBDIRTY(rest);
	    }
	    break;
	case TYPE_PLAYER:
	    if (Typeof(thing) == TYPE_PROGRAM && (FLAGS(rest) & INTERACTIVE)
		&& (PLAYER_CURR_PROG(rest) == thing)) {
		if (FLAGS(rest) & READMODE) {
		    notify(rest,
			   "The program you were running has been recycled.  Aborting program.");
		} else {
		    free_prog_text(PROGRAM_FIRST(thing));
		    PROGRAM_SET_FIRST(thing, NULL);
		    PLAYER_SET_INSERT_MODE(rest, 0);
		    FLAGS(thing) &= ~INTERNAL;
		    FLAGS(rest) &= ~INTERACTIVE;
		    PLAYER_SET_CURR_PROG(rest, NOTHING);
		    notify(rest,
			   "The program you were editing has been recycled.  Exiting Editor.");
		}
	    }
	    if (PLAYER_HOME(rest) == thing) {
		PLAYER_SET_HOME(rest, tp_player_start);
		DBDIRTY(rest);
	    }
	    if (EXITS(rest) == thing) {
		EXITS(rest) = NEXTOBJ(thing);
		DBDIRTY(rest);
	    }
	    if (PLAYER_CURR_PROG(rest) == thing)
		PLAYER_SET_CURR_PROG(rest, 0);
	    break;
	case TYPE_PROGRAM:
	    if (OWNER(rest) == thing) {
		OWNER(rest) = GOD;
		DBDIRTY(rest);
	    }
	}
	/*
	 *if (LOCATION(rest) == thing)
	 *    DBSTORE(rest, location, NOTHING);
	 */
	if (CONTENTS(rest) == thing)
	    DBSTORE(rest, contents, NEXTOBJ(thing));
	if (NEXTOBJ(rest) == thing)
	    DBSTORE(rest, next, NEXTOBJ(thing));
    }

    looplimit = db_top;
    while ((looplimit-- > 0) && ((first = CONTENTS(thing)) != NOTHING)) {
	if (Typeof(first) == TYPE_PLAYER ||
	    (Typeof(first) == TYPE_THING &&
	     (FLAGS(first) & (ZOMBIE | VEHICLE) || tp_secure_thing_movement))
		) {
	    enter_room(descr, first, HOME, LOCATION(thing));
	    /* If the room is set to drag players back, there'll be no
	     * reasoning with it.  DRAG the player out.
	     */
	    if (LOCATION(first) == thing) {
		notify(player, "Escaping teleport loop!  Going home.");
		moveto(first, HOME);
	    }
	} else {
	    moveto(first, HOME);
	}
    }


    moveto(thing, NOTHING);

    depth--;

    db_free_object(thing);
    db_clear_object(thing);
    NAME(thing) = strdup("<garbage>");
    SETDESC(thing, "<recyclable>");
    FLAGS(thing) = TYPE_GARBAGE;

    NEXTOBJ(thing) = recyclable;
    recyclable = thing;
    DBDIRTY(thing);
}
