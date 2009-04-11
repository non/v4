/*
 * File: dungeon.c
 * Purpose: The game core bits, shared across platforms.
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */
#include "angband.h"
#include "object/tvalsval.h"
#include "z-file.h"
#include "cmds.h"
#include "game-event.h"


/*
 * Regenerate hit points
 */
static void regenhp(int percent)
{
	s32b new_chp, new_chp_frac;
	int old_chp;

	/* Save the old hitpoints */
	old_chp = p_ptr->chp;

	/* Extract the new hitpoints */
	new_chp = ((long)p_ptr->mhp) * percent + PY_REGEN_HPBASE;
	p_ptr->chp += (s16b)(new_chp >> 16);   /* div 65536 */

	/* check for overflow */
	if ((p_ptr->chp < 0) && (old_chp > 0)) p_ptr->chp = MAX_SHORT;
	new_chp_frac = (new_chp & 0xFFFF) + p_ptr->chp_frac;	/* mod 65536 */
	if (new_chp_frac >= 0x10000L)
	{
		p_ptr->chp_frac = (u16b)(new_chp_frac - 0x10000L);
		p_ptr->chp++;
	}
	else
	{
		p_ptr->chp_frac = (u16b)new_chp_frac;
	}

	/* Fully healed */
	if (p_ptr->chp >= p_ptr->mhp)
	{
		p_ptr->chp = p_ptr->mhp;
		p_ptr->chp_frac = 0;
	}

	/* Notice changes */
	if (old_chp != p_ptr->chp)
	{
		/* Redraw */
		p_ptr->redraw |= (PR_HP);
	}
}


/*
 * Regenerate mana points
 */
static void regenmana(int percent)
{
	s32b new_mana, new_mana_frac;
	int old_csp;

	old_csp = p_ptr->csp;
	new_mana = ((long)p_ptr->msp) * percent + PY_REGEN_MNBASE;
	p_ptr->csp += (s16b)(new_mana >> 16);	/* div 65536 */
	/* check for overflow */
	if ((p_ptr->csp < 0) && (old_csp > 0))
	{
		p_ptr->csp = MAX_SHORT;
	}
	new_mana_frac = (new_mana & 0xFFFF) + p_ptr->csp_frac;	/* mod 65536 */
	if (new_mana_frac >= 0x10000L)
	{
		p_ptr->csp_frac = (u16b)(new_mana_frac - 0x10000L);
		p_ptr->csp++;
	}
	else
	{
		p_ptr->csp_frac = (u16b)new_mana_frac;
	}

	/* Must set frac to zero even if equal */
	if (p_ptr->csp >= p_ptr->msp)
	{
		p_ptr->csp = p_ptr->msp;
		p_ptr->csp_frac = 0;
	}

	/* Redraw mana */
	if (old_csp != p_ptr->csp)
	{
		/* Redraw */
		p_ptr->redraw |= (PR_MANA);
	}
}






/*
 * Regenerate the monsters (once per 100 game turns)
 *
 * XXX XXX XXX Should probably be done during monster turns.
 */
static void regen_monsters(void)
{
	int i, frac;

	/* Regenerate everyone */
	for (i = 1; i < mon_max; i++)
	{
		/* Check the i'th monster */
		monster_type *m_ptr = &mon_list[i];
		monster_race *r_ptr = &r_info[m_ptr->r_idx];

		/* Skip dead monsters */
		if (!m_ptr->r_idx) continue;

		/* Allow regeneration (if needed) */
		if (m_ptr->hp < m_ptr->maxhp)
		{
			/* Hack -- Base regeneration */
			frac = m_ptr->maxhp / 100;

			/* Hack -- Minimal regeneration rate */
			if (!frac) frac = 1;

			/* Hack -- Some monsters regenerate quickly */
			if (r_ptr->flags[1] & (RF1_REGENERATE)) frac *= 2;

			/* Hack -- Regenerate */
			m_ptr->hp += frac;

			/* Do not over-regenerate */
			if (m_ptr->hp > m_ptr->maxhp) m_ptr->hp = m_ptr->maxhp;

			/* Redraw (later) if needed */
			if (p_ptr->health_who == i) p_ptr->redraw |= (PR_HEALTH);
		}
	}
}


/*
 * If player has inscribed the object with "!!", let him know when it's
 * recharged. -LM-
 * Also inform player when first item of a stack has recharged. -HK-
 */
static void recharged_notice(const object_type *o_ptr, bool all)
{
	char o_name[120];

	cptr s;

	/* No inscription */
	if (!o_ptr->note) return;

	/* Find a '!' */
	s = strchr(quark_str(o_ptr->note), '!');

	/* Process notification request */
	while (s)
	{
		/* Find another '!' */
		if (s[1] == '!')
		{
			/* Describe (briefly) */
			object_desc(o_name, sizeof(o_name), o_ptr, FALSE, ODESC_BASE);

			/* Disturb the player */
			disturb(0, 0);

			/* Notify the player */
			if (o_ptr->number > 1)
			{
				if (all) msg_format("Your %s have recharged.", o_name);
				else msg_format("One of your %s has recharged.", o_name);
			}

			/* Artifacts */
			else if (o_ptr->name1)
			{
				msg_format("The %s has recharged.", o_name);
			}

			/* Single, non-artifact items */
			else msg_format("Your %s has recharged.", o_name);

			/* Done */
			return;
		}

		/* Keep looking for '!'s */
		s = strchr(s + 1, '!');
	}
}


/*
 * Recharge activatable objects in the player's equipment
 * and rods in the inventory and on the ground.
 */
static void recharge_objects(void)
{
	int i;

	int charged = 0;

	object_type *o_ptr;
	object_kind *k_ptr;


	/* Process equipment */
	for (i = INVEN_WIELD; i < INVEN_TOTAL; i++)
	{
		/* Get the object */
		o_ptr = &inventory[i];

		/* Skip non-objects */
		if (!o_ptr->k_idx) continue;

		/* Recharge activatable objects */
		if (o_ptr->timeout > 0 && !(o_ptr->tval == TV_LITE && !artifact_p(o_ptr)))
		{
			/* Recharge */
			o_ptr->timeout--;

			/* Notice changes */
			if (!o_ptr->timeout) charged++;

			/* Message if item is recharged, if inscribed with "!!" */
			if (!o_ptr->timeout) recharged_notice(o_ptr, TRUE);
		}
	}

	/* Notice changes */
	if (charged)
	{
		/* Window stuff */
		p_ptr->redraw |= (PR_EQUIP);
	}

	charged = 0;

	/* Recharge rods */
	for (i = 0; i < INVEN_PACK; i++)
	{
		o_ptr = &inventory[i];
		k_ptr = &k_info[o_ptr->k_idx];

		/* Skip non-objects */
		if (!o_ptr->k_idx) continue;

		/* Examine all charging rods */
		if ((o_ptr->tval == TV_ROD) && (o_ptr->timeout))
		{
			/* Determine how many rods are charging */
			int temp = (o_ptr->timeout + (k_ptr->time_base - 1)) / k_ptr->time_base;
			if (temp > o_ptr->number) temp = o_ptr->number;

			/* Decrease timeout by that number */
			o_ptr->timeout -= temp;

			/* Boundary control */
			if (o_ptr->timeout < 0) o_ptr->timeout = 0;

			/* Update if any rods are recharged */
			if (temp > (o_ptr->timeout + (k_ptr->time_base - 1)) / k_ptr->time_base)
			{
				/* Update window */
				charged++;

				/* Message if whole stack is recharged, if inscribed with "!!" */
				if (!o_ptr->timeout) recharged_notice(o_ptr, TRUE);
				/* Message if first in a stack is recharged, if inscribed with "!!" -HK- */
				else if (temp == o_ptr->number) recharged_notice(o_ptr, FALSE);
			}
		}
	}

	/* Notice changes */
	if (charged)
	{
		/* Combine pack */
		p_ptr->notice |= (PN_COMBINE);

		/* Redraw stuff */
		p_ptr->redraw |= (PR_INVEN);
	}


	/*** Process Objects ***/

	/* Process objects */
	for (i = 1; i < o_max; i++)
	{
		/* Get the object */
		o_ptr = &o_list[i];

		/* Skip dead objects */
		if (!o_ptr->k_idx) continue;

		/* Recharge rods on the ground */
		if ((o_ptr->tval == TV_ROD) && o_ptr->timeout)
		{
			/* Charge it */
			o_ptr->timeout -= o_ptr->number;

			/* Boundary control */
			if (o_ptr->timeout < 0) o_ptr->timeout = 0;
		}
	}
}


static void play_ambient_sound(void)
{
	/* Town sound */
	if (p_ptr->depth == 0) 
	{
		/* Hack - is it daytime or nighttime? */
		if (turn % (10L * TOWN_DAWN) < TOWN_DAWN / 2)
		{
			/* It's day. */
			sound(MSG_AMBIENT_DAY);
		} 
		else 
		{
			/* It's night. */
			sound(MSG_AMBIENT_NITE);
		}
		
	}

	/* Dungeon level 1-20 */
	else if (p_ptr->depth <= 20) 
	{
		sound(MSG_AMBIENT_DNG1);
	}

	/* Dungeon level 21-40 */
	else if (p_ptr->depth <= 40) 
	{
		sound(MSG_AMBIENT_DNG2);
	}

	/* Dungeon level 41-60 */
	else if (p_ptr->depth <= 60) 
	{
		sound(MSG_AMBIENT_DNG3);
	}

	/* Dungeon level 61-80 */
	else if (p_ptr->depth <= 80) 
	{
		sound(MSG_AMBIENT_DNG4);
	}

	/* Dungeon level 80- */
	else  
	{
		sound(MSG_AMBIENT_DNG5);
	}
}

/*
 * Helper for process_world -- decrement p_ptr->timed[] fields.
 */
static void decrease_timeouts(void)
{
	int adjust = (adj_con_fix[p_ptr->state.stat_ind[A_CON]] + 1);
	int i;

	/* Decrement all effects that can be done simply */
	for (i = 0; i < TMD_MAX; i++)
	{
		int decr = 1;
		if (!p_ptr->timed[i])
			continue;

		switch (i)
		{
			case TMD_CUT:
			{
				/* Hack -- check for truly "mortal" wound */
				decr = (p_ptr->timed[i] > 1000) ? 0 : adjust;
				break;
			}

			case TMD_POISONED:
			case TMD_STUN:
			{
				decr = adjust;
				break;
			}
		}
		/* Decrement the effect */
		dec_timed(i, decr, FALSE);
	}

	return;
}


/*
 * Handle certain things once every 10 game turns
 */
static void process_world(void)
{
	int i;

	int regen_amount;

	object_type *o_ptr;

	/* Every 10 game turns */
	if (turn % 10) return;


	/*** Check the Time and Load ***/

	/* Play an ambient sound at regular intervals. */
	if (!(turn % ((10L * TOWN_DAWN) / 4)))
	{
		play_ambient_sound();
	}

	/*** Handle the "town" (stores and sunshine) ***/

	/* While in town */
	if (!p_ptr->depth)
	{
		/* Hack -- Daybreak/Nighfall in town */
		if (!(turn % ((10L * TOWN_DAWN) / 2)))
		{
			bool dawn;

			/* Check for dawn */
			dawn = (!(turn % (10L * TOWN_DAWN)));

			/* Day breaks */
			if (dawn)
				msg_print("The sun has risen.");

			/* Night falls */
			else
				msg_print("The sun has fallen.");

			/* Illuminate */
			town_illuminate(dawn);
		}
	}


	/* While in the dungeon */
	else
	{
		/*** Update the Stores ***/

		/* Update the stores once a day (while in dungeon) */
		if (!(turn % (10L * STORE_TURNS)))
		{
			int n;

			/* Message */
			if (OPT(cheat_xtra)) msg_print("Updating Shops...");

			/* Maintain each shop (except home) */
			for (n = 0; n < MAX_STORES; n++)
			{
				/* Skip the home */
				if (n == STORE_HOME) continue;

				/* Maintain */
				store_maint(n);
			}

			/* Sometimes, shuffle the shop-keepers */
			if (one_in_(STORE_SHUFFLE))
			{
				/* Message */
				if (OPT(cheat_xtra)) msg_print("Shuffling a Shopkeeper...");

				/* Pick a random shop (except home) */
				while (1)
				{
					n = randint0(MAX_STORES);
					if (n != STORE_HOME) break;
				}

				/* Shuffle it */
				store_shuffle(n);
			}

			/* Message */
			if (OPT(cheat_xtra)) msg_print("Done.");
		}
	}


	/*** Process the monsters ***/

	/* Check for creature generation */
	if (one_in_(MAX_M_ALLOC_CHANCE))
	{
		/* Make a new monster */
		(void)alloc_monster(MAX_SIGHT + 5, FALSE, p_ptr->depth);
	}

	/* Hack -- Check for creature regeneration */
	if (!(turn % 100)) regen_monsters();


	/*** Damage over Time ***/

	/* Take damage from poison */
	if (p_ptr->timed[TMD_POISONED])
	{
		/* Take damage */
		take_hit(1, "poison");
	}

	/* Take damage from cuts */
	if (p_ptr->timed[TMD_CUT])
	{
		/* Mortal wound or Deep Gash */
		if (p_ptr->timed[TMD_CUT] > 200)
			i = 3;

		/* Severe cut */
		else if (p_ptr->timed[TMD_CUT] > 100)
			i = 2;

		/* Other cuts */
		else
			i = 1;

		/* Take damage */
		take_hit(i, "a fatal wound");
	}


	/*** Check the Food, and Regenerate ***/

	/* Digest normally */
	if (p_ptr->food < PY_FOOD_MAX)
	{
		/* Every 100 game turns */
		if (!(turn % 100))
		{
			/* Basic digestion rate based on speed */
			i = extract_energy[p_ptr->state.speed] * 2;

			/* Regeneration takes more food */
			if (p_ptr->state.regenerate) i += 30;

			/* Slow digestion takes less food */
			if (p_ptr->state.slow_digest) i -= 10;

			/* Minimal digestion */
			if (i < 1) i = 1;

			/* Digest some food */
			(void)set_food(p_ptr->food - i);
		}
	}

	/* Digest quickly when gorged */
	else
	{
		/* Digest a lot of food */
		(void)set_food(p_ptr->food - 100);
	}

	/* Getting Faint */
	if (p_ptr->food < PY_FOOD_FAINT)
	{
		/* Faint occasionally */
		if (!p_ptr->timed[TMD_PARALYZED] && one_in_(10))
		{
			/* Message */
			msg_print("You faint from the lack of food.");
			disturb(1, 0);

			/* Hack -- faint (bypass free action) */
			(void)inc_timed(TMD_PARALYZED, 1 + randint0(5), TRUE);
		}
	}


	/* Starve to death (slowly) */
	if (p_ptr->food < PY_FOOD_STARVE)
	{
		/* Calculate damage */
		i = (PY_FOOD_STARVE - p_ptr->food) / 10;

		/* Take damage */
		take_hit(i, "starvation");
	}

	/** Regenerate HP **/

	/* Default regeneration */
	if (p_ptr->food >= PY_FOOD_WEAK)
		regen_amount = PY_REGEN_NORMAL;
	else if (p_ptr->food < PY_FOOD_STARVE)
		regen_amount = 0;
	else if (p_ptr->food < PY_FOOD_FAINT)
		regen_amount = PY_REGEN_FAINT;
	else /* if (p_ptr->food < PY_FOOD_WEAK) */
		regen_amount = PY_REGEN_WEAK;

	/* Various things speed up regeneration */
	if (p_ptr->state.regenerate)
		regen_amount *= 2;
	if (p_ptr->searching || p_ptr->resting)
		regen_amount *= 2;

	/* Some things slow it down */
	if (p_ptr->state.impair_hp)
		regen_amount /= 2;

	/* Various things interfere with physical healing */
	if (p_ptr->timed[TMD_PARALYZED]) regen_amount = 0;
	if (p_ptr->timed[TMD_POISONED]) regen_amount = 0;
	if (p_ptr->timed[TMD_STUN]) regen_amount = 0;
	if (p_ptr->timed[TMD_CUT]) regen_amount = 0;

	/* Regenerate Hit Points if needed */
	if (p_ptr->chp < p_ptr->mhp)
		regenhp(regen_amount);


	/** Regenerate SP **/

	/* Default regeneration */
	regen_amount = PY_REGEN_NORMAL;

	/* Various things speed up regeneration */
	if (p_ptr->state.regenerate)
		regen_amount *= 2;
	if (p_ptr->searching || p_ptr->resting)
		regen_amount *= 2;

	/* Some things slow it down */
	if (p_ptr->state.impair_mana)
		regen_amount /= 2;

	/* Regenerate mana */
	if (p_ptr->csp < p_ptr->msp)
		regenmana(regen_amount);



	/*** Timeout Various Things ***/

	decrease_timeouts();



	/*** Process Light ***/

	/* Check for light being wielded */
	o_ptr = &inventory[INVEN_LITE];

	/* Burn some fuel in the current lite */
	if (o_ptr->tval == TV_LITE)
	{
		u32b f[OBJ_FLAG_N];
		bool burn_fuel = TRUE;

		/* Get the object flags */
		object_flags(o_ptr, f);

		/* Turn off the wanton burning of light during the day in the town */
		if (!p_ptr->depth && ((turn % (10L * TOWN_DAWN)) < ((10L * TOWN_DAWN) / 2)))
			burn_fuel = FALSE;

		/* If the light has the NO_FUEL flag, well... */
		if (f[2] & TR2_NO_FUEL)
		    burn_fuel = FALSE;

		/* Use some fuel (except on artifacts, or during the day) */
		if (burn_fuel && o_ptr->timeout > 0)
		{
			/* Decrease life-span */
			o_ptr->timeout--;

			/* Hack -- notice interesting fuel steps */
			if ((o_ptr->timeout < 100) || (!(o_ptr->timeout % 100)))
			{
				/* Redraw stuff */
				p_ptr->redraw |= (PR_EQUIP);
			}

			/* Hack -- Special treatment when blind */
			if (p_ptr->timed[TMD_BLIND])
			{
				/* Hack -- save some light for later */
				if (o_ptr->timeout == 0) o_ptr->timeout++;
			}

			/* The light is now out */
			else if (o_ptr->timeout == 0)
			{
				disturb(0, 0);
				msg_print("Your light has gone out!");
			}

			/* The light is getting dim */
			else if ((o_ptr->timeout < 100) && (!(o_ptr->timeout % 10)))
			{
				disturb(0, 0);
				msg_print("Your light is growing faint.");
			}
		}
	}

	/* Calculate torch radius */
	p_ptr->update |= (PU_TORCH);


	/*** Process Inventory ***/

	/* Handle experience draining */
	if (p_ptr->state.exp_drain)
	{
		if ((p_ptr->exp > 0) && one_in_(10))
		{
			p_ptr->exp--;
			p_ptr->max_exp--;
			check_experience();
		}

		object_notice_flag(3, TR2_DRAIN_EXP);
	}

	/* Recharge activatable objects and rods */
	recharge_objects();

	/* Feel the inventory */
	sense_inventory();


	/*** Involuntary Movement ***/

	/* Random teleportation */
	if (p_ptr->state.teleport && one_in_(100))
	{
		teleport_player(40);
		disturb(0, 0);
		object_notice_flag(3, TR2_TELEPORT);
	}

	/* Delayed Word-of-Recall */
	if (p_ptr->word_recall)
	{
		/* Count down towards recall */
		p_ptr->word_recall--;

		/* Activate the recall */
		if (!p_ptr->word_recall)
		{
			/* Disturbing! */
			disturb(0, 0);

			/* Sound */
			sound(MSG_TPLEVEL);

			/* Determine the level */
			if (p_ptr->depth)
			{
				msg_print("You feel yourself yanked upwards!");

				/* New depth */
				p_ptr->depth = 0;

				/* Leaving */
				p_ptr->leaving = TRUE;
			}
			else
			{
				msg_print("You feel yourself yanked downwards!");

				/* New depth */
				p_ptr->depth = p_ptr->max_depth;
				if (p_ptr->depth < 1) p_ptr->depth = 1;

				/* Leaving */
				p_ptr->leaving = TRUE;
			}
		}
	}
}





/*
 * Hack -- helper function for "process_player()"
 *
 * Check for changes in the "monster memory"
 */
static void process_player_aux(void)
{
	int i;
	bool changed = FALSE;

	static int old_monster_race_idx = 0;

	static u32b old_flags[RACE_FLAG_STRICT_UB] = { 0L, 0L, 0L };
	static u32b old_spell_flags[RACE_FLAG_SPELL_STRICT_UB] = { 0L, 0L, 0L };

	static byte old_blows[MONSTER_BLOW_MAX];

	static byte	old_cast_innate = 0;
	static byte	old_cast_spell = 0;


	/* Tracking a monster */
	if (p_ptr->monster_race_idx)
	{
		/* Get the monster lore */
		monster_lore *l_ptr = &l_list[p_ptr->monster_race_idx];

		for (i = 0; i < MONSTER_BLOW_MAX; i++)
		{
			if (old_blows[i] != l_ptr->blows[i])
			{
				changed = TRUE;
				break;
			}
		}

		/* Check for change of any kind */
		if (changed ||
		    (old_monster_race_idx != p_ptr->monster_race_idx) ||
		    race_flags_differ(old_flags, l_ptr->flags) ||
		    race_flags_differ_spell(old_spell_flags, l_ptr->spell_flags) ||
		    (old_cast_innate != l_ptr->cast_innate) ||
		    (old_cast_spell != l_ptr->cast_spell))
		{
			/* Memorize old race */
			old_monster_race_idx = p_ptr->monster_race_idx;

			/* Memorize flags */
			race_flags_assign(old_flags, l_ptr->flags);
			race_flags_assign_spell(old_spell_flags, l_ptr->spell_flags);

			/* Memorize blows */
			memmove(old_blows, l_ptr->blows, sizeof(byte)*MONSTER_BLOW_MAX);

			/* Memorize castings */
			old_cast_innate = l_ptr->cast_innate;
			old_cast_spell = l_ptr->cast_spell;

			/* Redraw stuff */
			p_ptr->redraw |= (PR_MONSTER);
			redraw_stuff();
		}
	}
}


/*
 * Process the player
 *
 * Notice the annoying code to handle "pack overflow", which
 * must come first just in case somebody manages to corrupt
 * the savefiles by clever use of menu commands or something.
 *
 * Notice the annoying code to handle "monster memory" changes,
 * which allows us to avoid having to update the window flags
 * every time we change any internal monster memory field, and
 * also reduces the number of times that the recall window must
 * be redrawn.
 *
 * Note that the code to check for user abort during repeated commands
 * and running and resting can be disabled entirely with an option, and
 * even if not disabled, it will only check during every 128th game turn
 * while resting, for efficiency.
 */
static void process_player(void)
{
	int i;


	/*** Check for interrupts ***/

	/* Complete resting */
	if (p_ptr->resting < 0)
	{
		/* Basic resting */
		if (p_ptr->resting == -1)
		{
			/* Stop resting */
			if ((p_ptr->chp == p_ptr->mhp) &&
			    (p_ptr->csp == p_ptr->msp))
			{
				disturb(0, 0);
			}
		}

		/* Complete resting */
		else if (p_ptr->resting == -2)
		{
			/* Stop resting */
			if ((p_ptr->chp == p_ptr->mhp) &&
			    (p_ptr->csp == p_ptr->msp) &&
			    !p_ptr->timed[TMD_BLIND] && !p_ptr->timed[TMD_CONFUSED] &&
			    !p_ptr->timed[TMD_POISONED] && !p_ptr->timed[TMD_AFRAID] &&
			    !p_ptr->timed[TMD_TERROR] &&
			    !p_ptr->timed[TMD_STUN] && !p_ptr->timed[TMD_CUT] &&
			    !p_ptr->timed[TMD_SLOW] && !p_ptr->timed[TMD_PARALYZED] &&
			    !p_ptr->timed[TMD_IMAGE] && !p_ptr->word_recall)
			{
				disturb(0, 0);
			}
		}
		
		/* Rest until HP or SP are filled */
		else if (p_ptr->resting == -3)
		{
			/* Stop resting */
			if ((p_ptr->chp == p_ptr->mhp) ||
			    (p_ptr->csp == p_ptr->msp))
			{
				disturb(0, 0);
			}
		}
	}

	/* Check for "player abort" */
	if (p_ptr->running ||
	    p_ptr->command_rep ||
	    (p_ptr->resting && !(turn & 0x7F)))
	{
		/* Do not wait */
		inkey_scan = SCAN_INSTANT;

		/* Check for a key */
		if (inkey())
		{
			/* Flush input */
			flush();

			/* Disturb */
			disturb(0, 0);

			/* Hack -- Show a Message */
			msg_print("Cancelled.");
		}
	}


	/*** Handle actual user input ***/

	/* Repeat until energy is reduced */
	do
	{
		/* Notice stuff (if needed) */
		if (p_ptr->notice) notice_stuff();

		/* Update stuff (if needed) */
		if (p_ptr->update) update_stuff();

		/* Redraw stuff (if needed) */
		if (p_ptr->redraw) redraw_stuff();


		/* Place the cursor on the player */
		move_cursor_relative(p_ptr->py, p_ptr->px);

		/* Refresh (optional) */
		Term_fresh();


		/* Hack -- Pack Overflow */
		if (inventory[INVEN_PACK].k_idx)
		{
			int item = INVEN_PACK;

			char o_name[80];

			object_type *o_ptr;

			/* Get the slot to be dropped */
			o_ptr = &inventory[item];

			/* Disturbing */
			disturb(0, 0);

			/* Warning */
			msg_print("Your pack overflows!");

			/* Describe */
			object_desc(o_name, sizeof(o_name), o_ptr, TRUE, ODESC_FULL);

			/* Message */
			msg_format("You drop %s (%c).", o_name, index_to_label(item));

			/* Drop it (carefully) near the player */
			drop_near(o_ptr, 0, p_ptr->py, p_ptr->px);

			/* Modify, Describe, Optimize */
			inven_item_increase(item, -255);
			inven_item_describe(item);
			inven_item_optimize(item);

			/* Notice stuff (if needed) */
			if (p_ptr->notice) notice_stuff();

			/* Update stuff (if needed) */
			if (p_ptr->update) update_stuff();

			/* Redraw stuff (if needed) */
			if (p_ptr->redraw) redraw_stuff();
		}


		/* Hack -- reset to inventory display */
		if (!p_ptr->command_new) p_ptr->command_wrk = USE_INVEN;


		/* Assume free turn */
		p_ptr->energy_use = 0;


		/* Paralyzed or Knocked Out */
		if ((p_ptr->timed[TMD_PARALYZED]) || (p_ptr->timed[TMD_STUN] >= 100))
		{
			/* Take a turn */
			p_ptr->energy_use = 100;
		}

		/* Picking up objects */
		else if (p_ptr->notice & (PN_PICKUP))
		{
			/* Recursively call the pickup function, use energy */
			p_ptr->energy_use = py_pickup(0) * 10;
			p_ptr->notice &= ~(PN_PICKUP);
		}

		/* Resting */
		else if (p_ptr->resting)
		{
			/* Timed rest */
			if (p_ptr->resting > 0)
			{
				/* Reduce rest count */
				p_ptr->resting--;

				/* Redraw the state */
				p_ptr->redraw |= (PR_STATE);
			}

			/* Take a turn */
			p_ptr->energy_use = 100;
		}

		/* Running */
		else if (p_ptr->running)
		{
			/* Take a step */
			run_step(0);
		}

		/* Repeated command */
		else if (p_ptr->command_rep)
		{
			/* Hack -- Assume messages were seen */
			msg_flag = FALSE;

			/* Clear the top line */
			prt("", 0, 0);

			/* Process the command */
			process_command(TRUE);

			/* Count this execution */
			if (p_ptr->command_rep)
			{
				/* Count this execution */
				p_ptr->command_rep--;

				/* Redraw the state */
				p_ptr->redraw |= (PR_STATE);
			}
		}

		/* Normal command */
		else
		{
			/* Check monster recall */
			process_player_aux();

			/* Place the cursor on the player */
			move_cursor_relative(p_ptr->py, p_ptr->px);

			/* Get and process a command */
			process_command(FALSE);
		}


		/*** Clean up ***/

		/* Significant */
		if (p_ptr->energy_use)
		{
			/* Use some energy */
			p_ptr->energy -= p_ptr->energy_use;


			/* Hack -- constant hallucination */
			if (p_ptr->timed[TMD_IMAGE])
			{
				p_ptr->redraw |= (PR_MAP);
			}

			/* Shimmer monsters if needed */
			if (shimmer_monsters)
			{
				/* Clear the flag */
				shimmer_monsters = FALSE;

				/* Shimmer multi-hued monsters */
				for (i = 1; i < mon_max; i++)
				{
					monster_type *m_ptr;
					monster_race *r_ptr;

					/* Get the monster */
					m_ptr = &mon_list[i];

					/* Skip dead monsters */
					if (!m_ptr->r_idx) continue;

					/* Get the monster race */
					r_ptr = &r_info[m_ptr->r_idx];

					/* Skip non-multi-hued monsters */
					if (!(r_ptr->flags[0] & (RF0_ATTR_MULTI))) continue;

					/* Reset the flag */
					shimmer_monsters = TRUE;

					/* Redraw regardless */
					lite_spot(m_ptr->fy, m_ptr->fx);
				}
			}

			/* Repair "nice" flags */
			if (repair_mflag_nice)
			{
				/* Clear flag */
				repair_mflag_nice = FALSE;

				/* Process monsters */
				for (i = 1; i < mon_max; i++)
				{
					monster_type *m_ptr;

					/* Get the monster */
					m_ptr = &mon_list[i];

					/* Skip dead monsters */
					/* if (!m_ptr->r_idx) continue; */

					/* Clear "nice" flag */
					m_ptr->mflag &= ~(MFLAG_NICE);
				}
			}

			/* Repair "mark" flags */
			if (repair_mflag_mark)
			{
				/* Reset the flag */
				repair_mflag_mark = FALSE;

				/* Process the monsters */
				for (i = 1; i < mon_max; i++)
				{
					monster_type *m_ptr;

					/* Get the monster */
					m_ptr = &mon_list[i];

					/* Skip dead monsters */
					/* if (!m_ptr->r_idx) continue; */

					/* Repair "mark" flag */
					if (m_ptr->mflag & (MFLAG_MARK))
					{
						/* Skip "show" monsters */
						if (m_ptr->mflag & (MFLAG_SHOW))
						{
							/* Repair "mark" flag */
							repair_mflag_mark = TRUE;

							/* Skip */
							continue;
						}

						/* Forget flag */
						m_ptr->mflag &= ~(MFLAG_MARK);

						/* Update the monster */
						update_mon(i, FALSE);
					}
				}
			}
		}

		/* Repair "show" flags */
		if (repair_mflag_show)
		{
			/* Reset the flag */
			repair_mflag_show = FALSE;

			/* Process the monsters */
			for (i = 1; i < mon_max; i++)
			{
				monster_type *m_ptr;

				/* Get the monster */
				m_ptr = &mon_list[i];

				/* Skip dead monsters */
				/* if (!m_ptr->r_idx) continue; */

				/* Clear "show" flag */
				m_ptr->mflag &= ~(MFLAG_SHOW);
			}
		}
		/* HACK: This will redraw the itemlist too frequently, but I'm don't
		   know all the individual places it should go. */
		p_ptr->redraw |= PR_ITEMLIST;
	}

	while (!p_ptr->energy_use && !p_ptr->leaving);
}



/*
 * Interact with the current dungeon level.
 *
 * This function will not exit until the level is completed,
 * the user dies, or the game is terminated.
 */
static void dungeon(void)
{
	monster_type *m_ptr;
	int i;



	/* Hack -- enforce illegal panel */
	Term->offset_y = DUNGEON_HGT;
	Term->offset_x = DUNGEON_WID;


	/* Not leaving */
	p_ptr->leaving = FALSE;


	/* Reset the "command" vars */
	p_ptr->command_cmd = 0;
	p_ptr->command_new = 0;
	p_ptr->command_rep = 0;
	p_ptr->command_arg = 0;
	p_ptr->command_dir = 0;


	/* Cancel the target */
	target_set_monster(0);

	/* Cancel the health bar */
	health_track(0);


	/* Reset shimmer flags */
	shimmer_monsters = TRUE;
	shimmer_objects = TRUE;

	/* Reset repair flags */
	repair_mflag_nice = TRUE;
	repair_mflag_show = TRUE;
	repair_mflag_mark = TRUE;


	/* Disturb */
	disturb(1, 0);


	/* Track maximum player level */
	if (p_ptr->max_lev < p_ptr->lev)
	{
		p_ptr->max_lev = p_ptr->lev;
	}


	/* Track maximum dungeon level */
	if (p_ptr->max_depth < p_ptr->depth)
	{
		p_ptr->max_depth = p_ptr->depth;
	}



	/* Choose panel */
	verify_panel();


	/* Flush messages */
	message_flush();


	/* Hack -- Increase "xtra" depth */
	character_xtra++;


	/* Clear */
	Term_clear();


	/* Update stuff */
	p_ptr->update |= (PU_BONUS | PU_HP | PU_MANA | PU_SPELLS);

	/* Calculate torch radius */
	p_ptr->update |= (PU_TORCH);

	/* Update stuff */
	update_stuff();


	/* Fully update the visuals (and monster distances) */
	p_ptr->update |= (PU_FORGET_VIEW | PU_UPDATE_VIEW | PU_DISTANCE);

	/* Fully update the flow */
	p_ptr->update |= (PU_FORGET_FLOW | PU_UPDATE_FLOW);

	/* Redraw dungeon */
	p_ptr->redraw |= (PR_BASIC | PR_EXTRA | PR_MAP);

	/* Redraw "statusy" things */
	p_ptr->redraw |= (PR_INVEN | PR_EQUIP | PR_MONSTER | PR_MONLIST | PR_ITEMLIST);

	/* Update stuff */
	update_stuff();

	/* Redraw stuff */
	redraw_stuff();


	/* Hack -- Decrease "xtra" depth */
	character_xtra--;


	/* Update stuff */
	p_ptr->update |= (PU_BONUS | PU_HP | PU_MANA | PU_SPELLS);

	/* Combine / Reorder the pack */
	p_ptr->notice |= (PN_COMBINE | PN_REORDER);

	/* Make basic mouse buttons */
	(void) button_add("[ESC]", ESCAPE);
	(void) button_add("[Ret]", '\r');
	(void) button_add("[Spc]", ' ');
	(void) button_add("[Rpt]", 'n');
	(void) button_add("[Std]", ',');

	/* Redraw buttons */
	p_ptr->redraw |= (PR_BUTTONS);

	/* Notice stuff */
	notice_stuff();

	/* Update stuff */
	update_stuff();

	/* Redraw stuff */
	redraw_stuff();

	/* Refresh */
	Term_fresh();


	/* Handle delayed death */
	if (p_ptr->is_dead) return;


	/* Announce (or repeat) the feeling */
	if (p_ptr->depth) do_cmd_feeling();


	/*** Process this dungeon level ***/

	/* Main loop */
	while (TRUE)
	{
		/* Hack -- Compact the monster list occasionally */
		if (mon_cnt + 32 > z_info->m_max) compact_monsters(64);

		/* Hack -- Compress the monster list occasionally */
		if (mon_cnt + 32 < mon_max) compact_monsters(0);


		/* Hack -- Compact the object list occasionally */
		if (o_cnt + 32 > z_info->o_max) compact_objects(64);

		/* Hack -- Compress the object list occasionally */
		if (o_cnt + 32 < o_max) compact_objects(0);


		/* Can the player move? */
		while ((p_ptr->energy >= 100) && !p_ptr->leaving)
		{
			/* process monster with even more energy first */
			process_monsters((byte)(p_ptr->energy + 1));

			/* if still alive */
			if (!p_ptr->leaving)
			{
				/* Process the player */
				process_player();
			}
		}

		/* Notice stuff */
		if (p_ptr->notice) notice_stuff();

		/* Update stuff */
		if (p_ptr->update) update_stuff();

		/* Redraw stuff */
		if (p_ptr->redraw) redraw_stuff();

		/* Hack -- Hilite the player */
		move_cursor_relative(p_ptr->py, p_ptr->px);

		/* Handle "leaving" */
		if (p_ptr->leaving) break;


		/* Process all of the monsters */
		process_monsters(100);

		/* Notice stuff */
		if (p_ptr->notice) notice_stuff();

		/* Update stuff */
		if (p_ptr->update) update_stuff();

		/* Redraw stuff */
		if (p_ptr->redraw) redraw_stuff();

		/* Hack -- Hilite the player */
		move_cursor_relative(p_ptr->py, p_ptr->px);

		/* Handle "leaving" */
		if (p_ptr->leaving) break;


		/* Process the world */
		process_world();

		/* Notice stuff */
		if (p_ptr->notice) notice_stuff();

		/* Update stuff */
		if (p_ptr->update) update_stuff();

		/* Redraw stuff */
		if (p_ptr->redraw) redraw_stuff();

		/* Hack -- Hilite the player */
		move_cursor_relative(p_ptr->py, p_ptr->px);

		/* Handle "leaving" */
		if (p_ptr->leaving) break;


		/*** Apply energy ***/

		/* Give the player some energy */
		p_ptr->energy += extract_energy[p_ptr->state.speed];

		/* Give energy to all monsters */
		for (i = mon_max - 1; i >= 1; i--)
		{
			/* Access the monster */
			m_ptr = &mon_list[i];

			/* Ignore "dead" monsters */
			if (!m_ptr->r_idx) continue;

			/* Give this monster some energy */
			m_ptr->energy += extract_energy[m_ptr->mspeed];
		}

		/* Count game turns */
		turn++;
	}
}



/*
 * Process some user pref files
 */
static void process_some_user_pref_files(void)
{
	char buf[1024];


	/* Process the "user.prf" file */
	(void)process_pref_file("user.prf");

	/* Get the "PLAYER.prf" filename */
	(void)strnfmt(buf, sizeof(buf), "%s.prf", op_ptr->base_name);

	/* Process the "PLAYER.prf" file */
	(void)process_pref_file(buf);
}


/*
 * Actually play a game.
 *
 * This function is called from a variety of entry points, since both
 * the standard "main.c" file, as well as several platform-specific
 * "main-xxx.c" files, call this function to start a new game with a
 * new savefile, start a new game with an existing savefile, or resume
 * a saved game with an existing savefile.
 *
 * If the "new_game" parameter is true, and the savefile contains a
 * living character, then that character will be killed, so that the
 * player may start a new game with that savefile.  This is only used
 * by the "-n" option in "main.c".
 *
 * If the savefile does not exist, cannot be loaded, or contains a dead
 * character, then a new game will be started.
 *
 * Several platforms (Windows, Macintosh, Amiga) start brand new games
 * with "savefile" and "op_ptr->base_name" both empty, and initialize
 * them later based on the player name.  To prevent weirdness, we must
 * initialize "op_ptr->base_name" to "PLAYER" if it is empty.
 *
 * Note that we load the RNG state from savefiles (2.8.0 or later) and
 * so we only initialize it if we were unable to load it.  The loading
 * code marks successful loading of the RNG state using the "Rand_quick"
 * flag, which is a hack, but which optimizes loading of savefiles.
 */
void play_game(void)
{
	bool existing_dead_save = FALSE;

	/* Initialize */
	bool new_game = init_angband();

	/*** Do horrible, hacky things, to start the game off ***/

	/* Hack -- Increase "icky" depth */
	character_icky++;

	/* Verify main term */
	if (!term_screen)
		quit("main window does not exist");

	/* Make sure main term is active */
	Term_activate(term_screen);

	/* Verify minimum size */
	if ((Term->hgt < 24) || (Term->wid < 80))
		quit("main window is too small");

	/* Hack -- Turn off the cursor */
	(void)Term_set_cursor(FALSE);


	/*** Try to load the savefile ***/

	p_ptr->is_dead = TRUE;

	if (savefile[0] && file_exists(savefile))
	{
		bool ok = old_load();
		if (!ok) quit("broken savefile");

		if (p_ptr->is_dead && arg_wizard)
		{
			p_ptr->is_dead = FALSE;
			p_ptr->noscore |= NOSCORE_WIZARD;
		}

		else if (p_ptr->is_dead)
		{
			existing_dead_save = TRUE;
		}
	}
	else
	{
		existing_dead_save = TRUE;
	}

	/* No living character loaded */
	if (p_ptr->is_dead)
	{
		/* Make new player */
		new_game = TRUE;

		/* The dungeon is not ready */
		character_dungeon = FALSE;

		/* XXX This is the place to add automatic character
		   numbering (i.e. Rocky IV, V, etc.) Probably. */
	}

	/* Hack -- Default base_name */
	if (!op_ptr->base_name[0])
		my_strcpy(op_ptr->base_name, "PLAYER", sizeof(op_ptr->base_name));


	/* Init RNG */
	if (Rand_quick)
	{
		u32b seed;

		/* Basic seed */
		seed = (time(NULL));

#ifdef SET_UID

		/* Mutate the seed on Unix machines */
		seed = ((seed >> 3) * (getpid() << 1));

#endif

		/* Use the complex RNG */
		Rand_quick = FALSE;

		/* Seed the "complex" RNG */
		Rand_state_init(seed);
	}

	/* Roll new character */
	if (new_game)
	{
		/* The dungeon is not ready */
		character_dungeon = FALSE;

		/* Start in town */
		p_ptr->depth = 0;

		/* Hack -- seed for flavors */
		seed_flavor = randint0(0x10000000);

		/* Hack -- seed for town layout */
		seed_town = randint0(0x10000000);

		/* Hack -- seed for random artifacts */
		seed_randart = randint0(0x10000000);

		/* Roll up a new character. Quickstart is allowed if ht_birth is set */
		player_birth(p_ptr->ht_birth ? TRUE : FALSE);

		/* Randomize the artifacts */
		if (OPT(adult_randarts))
			do_randart(seed_randart, TRUE);
	}


	/* Normal machine (process player name) */
	if (savefile[0])
		process_player_name(FALSE);

	/* Weird machine (process player name, pick savefile name) */
	else
		process_player_name(TRUE);

#if 0        
	/* Check if we're overwriting a savefile */
	while (new_game && !existing_dead_save)
	{
		bool overwrite = get_check("Continuing will overwrite an existing savefile.  Overwrite? ");

		if (overwrite) break;
		get_name(TRUE);
	}
#endif

	/* Stop the player being quite so dead */
	p_ptr->is_dead = FALSE;



	/* Flash a message */
	prt("Please wait...", 0, 0);


	/* Flush the message */
	Term_fresh();

	/* Flavor the objects */
	flavor_init();

	/* Reset visuals */
	reset_visuals(TRUE);

	/* Tell the UI we've started. */
	event_signal(EVENT_ENTER_GAME);

	/* Redraw stuff */
	p_ptr->redraw |= (PR_INVEN | PR_EQUIP | PR_MONSTER | PR_MESSAGE);
	redraw_stuff();


	/* Process some user pref files */
	process_some_user_pref_files();


	/* React to changes */
	Term_xtra(TERM_XTRA_REACT, 0);


	/* Generate a dungeon level if needed */
	if (!character_dungeon) generate_cave();


	/* Character is now "complete" */
	character_generated = TRUE;


	/* Hack -- Decrease "icky" depth */
	character_icky--;


	/* Start playing */
	p_ptr->playing = TRUE;

	/* Hack -- Enforce "delayed death" */
	if (p_ptr->chp < 0) p_ptr->is_dead = TRUE;

	/* Process */
	while (TRUE)
	{
		/* Play ambient sound on change of level. */
		play_ambient_sound();

		/* Process the level */
		dungeon();


		/* Notice stuff */
		if (p_ptr->notice) notice_stuff();

		/* Update stuff */
		if (p_ptr->update) update_stuff();

		/* Redraw stuff */
		if (p_ptr->redraw) redraw_stuff();


		/* Cancel the target */
		target_set_monster(0);

		/* Cancel the health bar */
		health_track(0);


		/* Forget the view */
		forget_view();


		/* Handle "quit and save" */
		if (!p_ptr->playing && !p_ptr->is_dead) break;


		/* XXX XXX XXX */
		message_flush();

		/* Accidental Death */
		if (p_ptr->playing && p_ptr->is_dead)
		{
			/* Mega-Hack -- Allow player to cheat death */
			if ((p_ptr->wizard || OPT(cheat_live)) && !get_check("Die? "))
			{
				/* Mark social class, reset age, if needed */
				if (p_ptr->sc) p_ptr->sc = p_ptr->age = 0;

				/* Increase age */
				p_ptr->age++;

				/* Mark savefile */
				p_ptr->noscore |= NOSCORE_WIZARD;

				/* Message */
				msg_print("You invoke wizard mode and cheat death.");
				message_flush();

				/* Cheat death */
				p_ptr->is_dead = FALSE;

				/* Restore hit points */
				p_ptr->chp = p_ptr->mhp;
				p_ptr->chp_frac = 0;

				/* Restore spell points */
				p_ptr->csp = p_ptr->msp;
				p_ptr->csp_frac = 0;

				/* Hack -- Healing */
				(void)clear_timed(TMD_BLIND, TRUE);
				(void)clear_timed(TMD_CONFUSED, TRUE);
				(void)clear_timed(TMD_POISONED, TRUE);
				(void)clear_timed(TMD_AFRAID, TRUE);
				(void)clear_timed(TMD_PARALYZED, TRUE);
				(void)clear_timed(TMD_IMAGE, TRUE);
				(void)clear_timed(TMD_STUN, TRUE);
				(void)clear_timed(TMD_CUT, TRUE);

				/* Hack -- Prevent starvation */
				(void)set_food(PY_FOOD_MAX - 1);

				/* Hack -- cancel recall */
				if (p_ptr->word_recall)
				{
					/* Message */
					msg_print("A tension leaves the air around you...");
					message_flush();

					/* Hack -- Prevent recall */
					p_ptr->word_recall = 0;
				}

				/* Note cause of death XXX XXX XXX */
				my_strcpy(p_ptr->died_from, "Cheating death", sizeof(p_ptr->died_from));

				/* New depth */
				p_ptr->depth = 0;

				/* Leaving */
				p_ptr->leaving = TRUE;
			}
		}

		/* Handle "death" */
		if (p_ptr->is_dead) break;

		/* Make a new level */
		generate_cave();
	}

	/* Tell the UI we're done with the game state */
	event_signal(EVENT_LEAVE_GAME);

	/* Close stuff */
	close_game();
}