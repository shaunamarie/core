/* 

   Copyright (C) Cfengine AS

   This file is part of Cfengine 3 - written and maintained by Cfengine AS.
 
   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
 
  You should have received a copy of the GNU General Public License  
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of Cfengine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

/*********************************************************************/
/*                                                                   */
/*  Variable expansion in cf3                                        */
/*                                                                   */
/*********************************************************************/

#include "expand.h"

#include "misc_lib.h"
#include "env_context.h"
#include "policy.h"
#include "promises.h"
#include "vars.h"
#include "syntax.h"
#include "files_names.h"
#include "conversion.h"
#include "reporting.h"
#include "scope.h"
#include "matching.h"
#include "unix.h"
#include "attributes.h"
#include "cfstream.h"
#include "fncall.h"
#include "args.h"
#include "logging.h"
#include "iteration.h"
#include "buffer.h"
#include "string_lib.h"

#ifdef HAVE_NOVA
#include "cf.nova.h"
#endif

#include <assert.h>

static void ExpandPromiseAndDo(EvalContext *ctx, AgentType agent, const Promise *pp, Rlist *listvars,
                               PromiseActuator *ActOnPromise, const ReportContext *report_context);
static void MapIteratorsFromScalar(const char *scope, Rlist **list_vars_out, char *string, int level);
static bool Epimenides(const char *scope, const char *var, Rval rval, int level);
static void RewriteInnerVarStringAsLocalCopyName(char *string);
static int CompareRlist(Rlist *list1, Rlist *list2);
static int CompareRval(Rval rval1, Rval rval2);
static void SetAnyMissingDefaults(EvalContext *ctx, Promise *pp);
static void CopyLocalizedIteratorsToThisScope(EvalContext *ctx, const char *scope, const Rlist *listvars);
static void CheckRecursion(EvalContext *ctx, const ReportContext *report_context, Promise *pp);
static void ParseServices(EvalContext *ctx, const ReportContext *report_context, Promise *pp);
/*

Expanding variables is easy -- expanding lists automagically requires
some thought. Remember that

promiser <=> RVAL_TYPE_SCALAR
promisee <=> RVAL_TYPE_LIST

Expanding all bodies in the constraint list, we have

lval <=> RVAL_TYPE_LIST|RVAL_TYPE_SCALAR

Now the rule for variable substitution is that any list variable @(name)
substituted directly for a LIST is not iterated, but dropped into
place, i.e. in list-lvals and the promisee (since this would be
equivalent to a re-concatenation of the expanded separate promises)

Any list variable occuring within a scalar or in place of a scalar
is assumed to be iterated i.e. $(name).

To expand a promise, we build temporary hash tables. There are two
stages, to this - one is to create a promise copy including all of the
body templates and translate the parameters. This requires one round
of expansion with scopeid "body". Then we use this fully assembled promise
and expand vars and function calls.

To expand the variables in a promise we need to 

   -- first get all strings, also parameterized bodies, which
      could also be lists
                                                                     /
        //  MapIteratorsFromRval("scope",&lol,"ksajd$(one)$(two)...$(three)"); \/ 
        
   -- compile an ordered list of variables involved , with types -           /
      assume all are lists - these are never inside sub-bodies by design,  \/
      so all expansion data are in the promise itself
      can also be variables based on list items - derived like arrays x[i]

   -- Copy the promise to a temporary promise + constraint list, expanding one by one,   /
      then execute that                                                                \/

      -- In a sub-bundle, create a new context and make hashes of the the
      transferred variables in the temporary context

   -- bodies cannot contain iterators

   -- we've already checked types of lhs rhs, must match so an iterator
      can only be in a non-naked variable?
   
   -- form the outer loops to generate combinations

Note, we map the current context into a fluid context "this" that maps
every list into a scalar during iteration. Thus "this" never contains
lists. This presents a problem for absolute references like $(abs.var),
since these cannot be mapped into "this" without some magic.
   
**********************************************************************/

void ExpandPromise(EvalContext *ctx, AgentType agent, Promise *pp, PromiseActuator *ActOnPromise, const ReportContext *report_context)
{
    Rlist *listvars = NULL;
    Promise *pcopy;

    CfDebug("****************************************************\n");
    CfDebug("* ExpandPromises (scope = %s )\n", PromiseGetBundle(pp)->name);
    CfDebug("****************************************************\n\n");

// Set a default for packages here...general defaults that need to come before

//fix me wth a general function SetMissingDefaults

    SetAnyMissingDefaults(ctx, pp);

    ScopeClear("match");       /* in case we expand something expired accidentially */

    EvalContextStackPushPromiseFrame(ctx, pp);

    pcopy = DeRefCopyPromise(ctx, pp);

    EvalContextStackPopFrame(ctx);

    MapIteratorsFromRval(PromiseGetBundle(pp)->name, &listvars, (Rval) { pcopy->promiser, RVAL_TYPE_SCALAR });

    if (pcopy->promisee.item != NULL)
    {
        MapIteratorsFromRval(PromiseGetBundle(pp)->name, &listvars, pp->promisee);
    }

    for (size_t i = 0; i < SeqLength(pcopy->conlist); i++)
    {
        Constraint *cp = SeqAt(pcopy->conlist, i);
        MapIteratorsFromRval(PromiseGetBundle(pp)->name, &listvars, cp->rval);
    }

    CopyLocalizedIteratorsToThisScope(ctx, PromiseGetBundle(pp)->name, listvars);

    ScopePushThis();
    ExpandPromiseAndDo(ctx, agent, pcopy, listvars, ActOnPromise, report_context);
    ScopePopThis();

    PromiseDestroy(pcopy);
    RlistDestroy(listvars);
}

/*********************************************************************/

Rval ExpandDanglers(EvalContext *ctx, const char *scopeid, Rval rval, const Promise *pp)
{
    Rval final;

    /* If there is still work left to do, expand and replace alloc */

    switch (rval.type)
    {
    case RVAL_TYPE_SCALAR:

        if (IsCf3VarString(rval.item))
        {
            final = EvaluateFinalRval(ctx, scopeid, rval, false, pp);
        }
        else
        {
            final = RvalCopy(rval);
        }
        break;

    default:
        final = RvalCopy(rval);
        break;
    }

    return final;
}

/*********************************************************************/

void MapIteratorsFromRval(const char *scopeid, Rlist **listvars, Rval rval)
{
    Rlist *rp;
    FnCall *fp;

    if (rval.item == NULL)
    {
        return;
    }

    switch (rval.type)
    {
    case RVAL_TYPE_SCALAR:
        MapIteratorsFromScalar(scopeid, listvars, (char *) rval.item, 0);
        break;

    case RVAL_TYPE_LIST:
        for (rp = (Rlist *) rval.item; rp != NULL; rp = rp->next)
        {
            MapIteratorsFromRval(scopeid, listvars, (Rval) {rp->item, rp->type});
        }
        break;

    case RVAL_TYPE_FNCALL:
        fp = (FnCall *) rval.item;

        for (rp = (Rlist *) fp->args; rp != NULL; rp = rp->next)
        {
            CfDebug("Looking at arg for function-like object %s()\n", fp->name);
            MapIteratorsFromRval(scopeid, listvars, (Rval) {rp->item, rp->type});
        }
        break;

    default:
        CfDebug("Unknown Rval type for scope %s", scopeid);
        break;
    }
}

/*********************************************************************/

static void MapIteratorsFromScalar(const char *scopeid, Rlist **list_vars_out, char *string, int level)
{
    char *sp;
    Rval rval;
    char v[CF_BUFSIZE], var[CF_EXPANDSIZE], exp[CF_EXPANDSIZE], temp[CF_BUFSIZE], finalname[CF_BUFSIZE];

    CfDebug("MapIteratorsFromScalar(\"%s\", %d)\n", string, level);

    if (string == NULL)
    {
        return;
    }

    for (sp = string; (*sp != '\0'); sp++)
    {
        v[0] = '\0';
        var[0] = '\0';
        exp[0] = '\0';

        if (*sp == '$')
        {
            if (ExtractInnerCf3VarString(sp, v))
            {
                char absscope[CF_MAXVARSIZE];
                int qualified;

                // If a list is non-local, i.e. $(bundle.var), map it to local $(bundle#var)

                if (IsQualifiedVariable(v))
                {
                    strncpy(temp, v, CF_BUFSIZE - 1);
                    absscope[0] = '\0';
                    sscanf(temp, "%[^.].%s", absscope, v);
                    ExpandPrivateScalar(absscope, v, var);
                    snprintf(finalname, CF_MAXVARSIZE, "%s%c%s", absscope, CF_MAPPEDLIST, var);
                    qualified = true;
                }
                else
                {
                    strncpy(absscope, scopeid, CF_MAXVARSIZE - 1);
                    ExpandPrivateScalar(absscope, v, var);
                    strncpy(finalname, var, CF_BUFSIZE - 1);
                    qualified = false;
                }

                // var is the expanded name of the variable in its native context
                // finalname will be the mapped name in the local context "this."

                if (ScopeGetVariable((VarRef) { NULL, absscope, var }, &rval) != DATA_TYPE_NONE)
                {
                    if (rval.type == RVAL_TYPE_LIST)
                    {
                        ExpandScalar(scopeid, finalname, exp);

                        if (qualified)
                        {
                            RewriteInnerVarStringAsLocalCopyName(sp);
                        }

                        /* embedded iterators should be incremented fastest,
                           so order list -- and MUST return de-scoped name
                           else list expansion cannot map var to this.name */

                        if (level > 0)
                        {
                            RlistPrependScalarIdemp(list_vars_out, exp);
                        }
                        else
                        {
                            RlistAppendScalarIdemp(list_vars_out, exp);
                        }
                    }
                }
                else
                {
                    CfDebug("Checking for nested vars, e.g. $(array[$(index)])....\n");

                    if (IsExpandable(var))
                    {
                        MapIteratorsFromScalar(scopeid, list_vars_out, var, level + 1);

                        // Need to rewrite list references to nested variables in this level

                        if (strchr(var, CF_MAPPEDLIST))
                        {
                            // Qualified outer var will eat the rewrite
                            if (qualified)
                            {
                                // Skip parent scope and '$(.'
                                RewriteInnerVarStringAsLocalCopyName(sp + strlen(absscope) + 3);
                            }
                            else
                            {
                                RewriteInnerVarStringAsLocalCopyName(sp);
                            }
                        }
                    }
                }

                sp += strlen(var) - 1;
            }
        }
    }
}

/*********************************************************************/

int ExpandScalar(const char *scope, const char *string, char buffer[CF_EXPANDSIZE])
{
    return ExpandPrivateScalar(scope, string, buffer);
}

/*********************************************************************/

Rlist *ExpandList(const char *scopeid, const Rlist *list, int expandnaked)
{
    Rlist *rp, *start = NULL;
    Rval returnval;
    char naked[CF_MAXVARSIZE];

    for (rp = (Rlist *) list; rp != NULL; rp = rp->next)
    {
        if (!expandnaked && (rp->type == RVAL_TYPE_SCALAR) && IsNakedVar(rp->item, '@'))
        {
            returnval.item = xstrdup(rp->item);
            returnval.type = RVAL_TYPE_SCALAR;
        }
        else if ((rp->type == RVAL_TYPE_SCALAR) && IsNakedVar(rp->item, '@'))
        {
            GetNaked(naked, rp->item);

            if (ScopeGetVariable((VarRef) { NULL, scopeid, naked }, &returnval) != DATA_TYPE_NONE)
            {
                returnval = ExpandPrivateRval(scopeid, returnval);
            }
            else
            {
                returnval = ExpandPrivateRval(scopeid, (Rval) {rp->item, rp->type});
            }
        }
        else
        {
            returnval = ExpandPrivateRval(scopeid, (Rval) {rp->item, rp->type});
        }

        RlistAppend(&start, returnval.item, returnval.type);
        RvalDestroy(returnval);
    }

    return start;
}

/*********************************************************************/

Rval ExpandPrivateRval(const char *scopeid, Rval rval)
{
    char buffer[CF_EXPANDSIZE];
    FnCall *fp, *fpe;
    Rval returnval;

    CfDebug("ExpandPrivateRval(scope=%s,type=%c)\n", scopeid, rval.type);

/* Allocates new memory for the copy */

    returnval.item = NULL;
    returnval.type = RVAL_TYPE_NOPROMISEE;

    switch (rval.type)
    {
    case RVAL_TYPE_SCALAR:

        ExpandPrivateScalar(scopeid, (char *) rval.item, buffer);
        returnval.item = xstrdup(buffer);
        returnval.type = RVAL_TYPE_SCALAR;
        break;

    case RVAL_TYPE_LIST:

        returnval.item = ExpandList(scopeid, rval.item, true);
        returnval.type = RVAL_TYPE_LIST;
        break;

    case RVAL_TYPE_FNCALL:

        /* Note expand function does not mean evaluate function, must preserve type */
        fp = (FnCall *) rval.item;
        fpe = ExpandFnCall(scopeid, fp, true);
        returnval.item = fpe;
        returnval.type = RVAL_TYPE_FNCALL;
        break;

    default:
        break;
    }

    return returnval;
}

/*********************************************************************/

Rval ExpandBundleReference(const char *scopeid, Rval rval)
{
    CfDebug("ExpandBundleReference(scope=%s,type=%c)\n", scopeid, rval.type);

/* Allocates new memory for the copy */

    switch (rval.type)
    {
    case RVAL_TYPE_SCALAR:
    {
        char buffer[CF_EXPANDSIZE];

        ExpandPrivateScalar(scopeid, (char *) rval.item, buffer);
        return (Rval) {xstrdup(buffer), RVAL_TYPE_SCALAR};
    }

    case RVAL_TYPE_FNCALL:
    {
        /* Note expand function does not mean evaluate function, must preserve type */
        FnCall *fp = (FnCall *) rval.item;

        return (Rval) {ExpandFnCall(scopeid, fp, false), RVAL_TYPE_FNCALL};
    }

    default:
        return (Rval) {NULL, RVAL_TYPE_NOPROMISEE };
    }
}

/*********************************************************************/

static bool ExpandOverflow(const char *str1, const char *str2)
{
    int len = strlen(str2);

    if ((strlen(str1) + len) > (CF_EXPANDSIZE - CF_BUFFERMARGIN))
    {
        CfOut(OUTPUT_LEVEL_ERROR, "",
              "Expansion overflow constructing string. Increase CF_EXPANDSIZE macro. Tried to add %s to %s\n", str2,
              str1);
        return true;
    }

    return false;
}

/*********************************************************************/

int ExpandPrivateScalar(const char *scopeid, const char *string, char buffer[CF_EXPANDSIZE])
{
    const char *sp;
    Rval rval;
    int varstring = false;
    char currentitem[CF_EXPANDSIZE], temp[CF_BUFSIZE], name[CF_MAXVARSIZE];
    int increment, returnval = true;

    buffer[0] = '\0';

    if (string == 0 || strlen(string) == 0)
    {
        return false;
    }

    CfDebug("\nExpandPrivateScalar(%s,%s)\n", scopeid, string);

    for (sp = string; /* No exit */ ; sp++)     /* check for varitems */
    {
        char var[CF_BUFSIZE];

        var[0] = '\0';

        increment = 0;

        if (*sp == '\0')
        {
            break;
        }

        currentitem[0] = '\0';

        sscanf(sp, "%[^$]", currentitem);

        if (ExpandOverflow(buffer, currentitem))
        {
            FatalError("Can't expand varstring");
        }

        strlcat(buffer, currentitem, CF_EXPANDSIZE);
        sp += strlen(currentitem);

        CfDebug("  Aggregate result |%s|, scanning at \"%s\" (current delta %s)\n", buffer, sp, currentitem);

        if (*sp == '\0')
        {
            break;
        }

        if (*sp == '$')
        {
            switch (*(sp + 1))
            {
            case '(':
                ExtractOuterCf3VarString(sp, var);
                varstring = ')';
                if (strlen(var) == 0)
                {
                    strlcat(buffer, "$", CF_EXPANDSIZE);
                    continue;
                }
                break;

            case '{':
                ExtractOuterCf3VarString(sp, var);
                varstring = '}';
                if (strlen(var) == 0)
                {
                    strlcat(buffer, "$", CF_EXPANDSIZE);
                    continue;
                }
                break;

            default:
                strlcat(buffer, "$", CF_EXPANDSIZE);
                continue;
            }
        }

        currentitem[0] = '\0';

        temp[0] = '\0';
        ExtractInnerCf3VarString(sp, temp);

        if (IsCf3VarString(temp))
        {
            CfDebug("  Nested variables - %s\n", temp);
            ExpandPrivateScalar(scopeid, temp, currentitem);
        }
        else
        {
            CfDebug("  Delta - %s\n", temp);
            strncpy(currentitem, temp, CF_BUFSIZE - 1);
        }

        increment = strlen(var) - 1;

        switch (ScopeGetVariable((VarRef) { NULL, scopeid, currentitem }, &rval))
        {
        case DATA_TYPE_STRING:
        case DATA_TYPE_INT:
        case DATA_TYPE_REAL:

            if (ExpandOverflow(buffer, (char *) rval.item))
            {
                FatalError("Can't expand varstring");
            }

            strlcat(buffer, (char *) rval.item, CF_EXPANDSIZE);
            break;

        case DATA_TYPE_STRING_LIST:
        case DATA_TYPE_INT_LIST:
        case DATA_TYPE_REAL_LIST:
        case DATA_TYPE_NONE:
            CfDebug("  Currently non existent or list variable $(%s)\n", currentitem);

            if (varstring == '}')
            {
                snprintf(name, CF_MAXVARSIZE, "${%s}", currentitem);
            }
            else
            {
                snprintf(name, CF_MAXVARSIZE, "$(%s)", currentitem);
            }

            strlcat(buffer, name, CF_EXPANDSIZE);
            returnval = false;
            break;

        default:
            CfDebug("Returning Unknown Scalar (%s => %s)\n\n", string, buffer);
            return false;

        }

        sp += increment;
        currentitem[0] = '\0';
    }

    if (returnval)
    {
        CfDebug("Returning complete scalar expansion (%s => %s)\n\n", string, buffer);

        /* Can we be sure this is complete? What about recursion */
    }
    else
    {
        CfDebug("Returning partial / best effort scalar expansion (%s => %s)\n\n", string, buffer);
    }

    return returnval;
}

/*********************************************************************/

static void ExpandPromiseAndDo(EvalContext *ctx, AgentType agent, const Promise *pp, Rlist *listvars, PromiseActuator *ActOnPromise, const ReportContext *report_context)
{
    Rlist *lol = NULL;
    Promise *pexp;
    const int cf_null_cutoff = 5;
    const char *handle = PromiseGetHandle(pp);
    char v[CF_MAXVARSIZE];
    int cutoff = 0;

    lol = NewIterationContext(ctx, PromiseGetBundle(pp)->name, listvars);

    if (lol && EndOfIteration(lol))
    {
        DeleteIterationContext(lol);
        return;
    }

    while (NullIterators(lol))
    {
        IncrementIterationContext(lol);

        // In case a list is completely blank
        if (cutoff++ > cf_null_cutoff)
        {
            break;
        }
    }

    if (lol && EndOfIteration(lol))
    {
        DeleteIterationContext(lol);
        return;
    }

    do
    {
        char number[CF_SMALLBUF];

        /* Set scope "this" first to ensure list expansion ! */
        EvalContextStackPushPromiseFrame(ctx, pp);
        ScopeDeRefListsInHashtable("this", listvars, lol);

        /* Allow $(this.handle) etc variables */

        if (handle)
        {
            char tmp[CF_EXPANDSIZE];
            // This ordering is necessary to get automated canonification
            ExpandScalar("this", handle, tmp);
            CanonifyNameInPlace(tmp);
            ScopeNewSpecialScalar(ctx, "this", "handle", tmp, DATA_TYPE_STRING);
        }
        else
        {
            ScopeNewSpecialScalar(ctx, "this", "handle", PromiseID(pp), DATA_TYPE_STRING);
        }

        if (PromiseGetBundle(pp)->source_path)
        {
            ScopeNewSpecialScalar(ctx, "this", "promise_filename",PromiseGetBundle(pp)->source_path, DATA_TYPE_STRING);
            snprintf(number, CF_SMALLBUF, "%zu", pp->offset.line);
            ScopeNewSpecialScalar(ctx, "this", "promise_linenumber", number, DATA_TYPE_STRING);
        }

        snprintf(v, CF_MAXVARSIZE, "%d", (int) getuid());
        ScopeNewSpecialScalar(ctx, "this", "promiser_uid", v, DATA_TYPE_INT);
        snprintf(v, CF_MAXVARSIZE, "%d", (int) getgid());
        ScopeNewSpecialScalar(ctx, "this", "promiser_gid", v, DATA_TYPE_INT);

        ScopeNewSpecialScalar(ctx, "this", "bundle", PromiseGetBundle(pp)->name, DATA_TYPE_STRING);
        ScopeNewSpecialScalar(ctx, "this", "namespace", PromiseGetNamespace(pp), DATA_TYPE_STRING);

        /* Must expand $(this.promiser) here for arg dereferencing in things
           like edit_line and methods, but we might have to
           adjust again later if the value changes  -- need to qualify this
           so we don't expand too early for some other promsies */

        if (pp->has_subbundles)
        {
            ScopeNewSpecialScalar(ctx, "this", "promiser", pp->promiser, DATA_TYPE_STRING);
        }

        /* End special variables */

        pexp = ExpandDeRefPromise(ctx, "this", pp);

        switch (agent)
        {
        case AGENT_TYPE_COMMON:
            ShowPromise(pexp);
            CheckRecursion(ctx, report_context, pexp);
            PromiseRecheckAllConstraints(ctx, pexp);
            break;

        default:
            assert(ActOnPromise);
            if (ActOnPromise)
            {
                ActOnPromise(ctx, pexp, report_context);
            }
            break;
        }

        if (strcmp(pp->parent_promise_type->name, "vars") == 0 || strcmp(pp->parent_promise_type->name, "meta") == 0)
        {
            ConvergeVarHashPromise(ctx, pexp, true);
        }
        
        PromiseDestroy(pexp);

        EvalContextStackPopFrame(ctx);
        /* End thread monitor */
    }
    while (IncrementIterationContext(lol));

    DeleteIterationContext(lol);
}

/*********************************************************************/

Rval EvaluateFinalRval(EvalContext *ctx, const char *scopeid, Rval rval, int forcelist, const Promise *pp)
{
    Rlist *rp;
    Rval returnval, newret;
    char naked[CF_MAXVARSIZE];
    FnCall *fp;

    CfDebug("EvaluateFinalRval -- type %c\n", rval.type);

    if ((rval.type == RVAL_TYPE_SCALAR) && IsNakedVar(rval.item, '@'))        /* Treat lists specially here */
    {
        GetNaked(naked, rval.item);

        if (ScopeGetVariable((VarRef) { NULL, scopeid, naked }, &returnval) == DATA_TYPE_NONE || returnval.type != RVAL_TYPE_LIST)
        {
            returnval = ExpandPrivateRval("this", rval);
        }
        else
        {
            returnval.item = ExpandList(scopeid, returnval.item, true);
            returnval.type = RVAL_TYPE_LIST;
        }
    }
    else
    {
        if (forcelist)          /* We are replacing scalar @(name) with list */
        {
            returnval = ExpandPrivateRval(scopeid, rval);
        }
        else
        {
            if (FnCallIsBuiltIn(rval))
            {
                returnval = RvalCopy(rval);
            }
            else
            {
                returnval = ExpandPrivateRval("this", rval);
            }
        }
    }

    switch (returnval.type)
    {
    case RVAL_TYPE_SCALAR:
        break;

    case RVAL_TYPE_LIST:
        for (rp = (Rlist *) returnval.item; rp != NULL; rp = rp->next)
        {
            if (rp->type == RVAL_TYPE_FNCALL)
            {
                fp = (FnCall *) rp->item;
                FnCallResult res = FnCallEvaluate(ctx, fp, pp);

                FnCallDestroy(fp);
                rp->item = res.rval.item;
                rp->type = res.rval.type;
                CfDebug("Replacing function call with new type (%c)\n", rp->type);
            }
            else
            {
                if (ScopeExists("this"))
                {
                    if (IsCf3VarString(rp->item))
                    {
                        newret = ExpandPrivateRval("this", (Rval) {rp->item, rp->type});
                        free(rp->item);
                        rp->item = newret.item;
                    }
                }
            }

            /* returnval unchanged */
        }
        break;

    case RVAL_TYPE_FNCALL:

        // Also have to eval function now
        fp = (FnCall *) returnval.item;
        returnval = FnCallEvaluate(ctx, fp, pp).rval;
        FnCallDestroy(fp);
        break;

    default:
        returnval.item = NULL;
        returnval.type = RVAL_TYPE_NOPROMISEE;
        break;
    }

    return returnval;
}

/*********************************************************************/

static void RewriteInnerVarStringAsLocalCopyName(char *string)
{
    char *sp;

    for (sp = string; *sp != '\0'; sp++)
    {
        if (*sp == '.')
        {
            *sp = CF_MAPPEDLIST;
            return;
        }
    }
}

/*********************************************************************/

static void CopyLocalizedIteratorsToThisScope(EvalContext *ctx, const char *scope, const Rlist *listvars)
{
    Rval retval;
    char format[CF_SMALLBUF];

    for (const Rlist *rp = listvars; rp != NULL; rp = rp->next)
    {
        // Add re-mapped variables to context "this", marked with scope . -> #

        if (strchr(rp->item, '#'))
        {
            char orgscope[CF_MAXVARSIZE], orgname[CF_MAXVARSIZE];

            snprintf(format, CF_SMALLBUF, "%%[^%c]%c%%s", CF_MAPPEDLIST, CF_MAPPEDLIST);

            sscanf(rp->item, format, orgscope, orgname);

            ScopeGetVariable((VarRef) { NULL, orgscope, orgname }, &retval);

            ScopeNewList(ctx, (VarRef) { NULL, scope, rp->item }, RvalCopy((Rval) {retval.item, RVAL_TYPE_LIST}).item, DATA_TYPE_STRING_LIST);
        }
    }
}

/*********************************************************************/
/* Tools                                                             */
/*********************************************************************/

int IsExpandable(const char *str)
{
    const char *sp;
    char left = 'x', right = 'x';
    int dollar = false;
    int bracks = 0, vars = 0;

    CfDebug("IsExpandable(%s) - syntax verify\n", str);

    for (sp = str; *sp != '\0'; sp++)   /* check for varitems */
    {
        switch (*sp)
        {
        case '$':
            if (*(sp + 1) == '{' || *(sp + 1) == '(')
            {
                dollar = true;
            }
            break;
        case '(':
        case '{':
            if (dollar)
            {
                left = *sp;
                bracks++;
            }
            break;
        case ')':
        case '}':
            if (dollar)
            {
                bracks--;
                right = *sp;
            }
            break;
        }

        if (left == '(' && right == ')' && dollar && (bracks == 0))
        {
            vars++;
            dollar = false;
        }

        if (left == '{' && right == '}' && dollar && (bracks == 0))
        {
            vars++;
            dollar = false;
        }
    }

    if (bracks != 0)
    {
        CfDebug("If this is an expandable variable string then it contained syntax errors");
        return false;
    }

    CfDebug("Found %d variables in (%s)\n", vars, str);
    return vars;
}

/*********************************************************************/

int IsNakedVar(const char *str, char vtype)
{
    int count = 0;

    if (str == NULL || strlen(str) == 0)
    {
        return false;
    }

    char last = *(str + strlen(str) - 1);

    if (strlen(str) < 3)
    {
        return false;
    }

    if (*str != vtype)
    {
        return false;
    }

    switch (*(str + 1))
    {
    case '(':
        if (last != ')')
        {
            return false;
        }
        break;

    case '{':
        if (last != '}')
        {
            return false;
        }
        break;

    default:
        return false;
        break;
    }

    for (const char *sp = str; *sp != '\0'; sp++)
    {
        switch (*sp)
        {
        case '(':
        case '{':
        case '[':
            count++;
            break;
        case ')':
        case '}':
        case ']':
            count--;

            /* The last character must be the end of the variable */

            if (count == 0 && strlen(sp) > 1)
            {
                return false;
            }
            break;
        }
    }

    if (count != 0)
    {
        return false;
    }

    CfDebug("IsNakedVar(%s,%c)!!\n", str, vtype);
    return true;
}

/*********************************************************************/

void GetNaked(char *s2, const char *s1)
/* copy @(listname) -> listname */
{
    if (strlen(s1) < 4)
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Naked variable expected, but \"%s\" is malformed", s1);
        strncpy(s2, s1, CF_MAXVARSIZE - 1);
        return;
    }

    memset(s2, 0, CF_MAXVARSIZE);
    strncpy(s2, s1 + 2, strlen(s1) - 3);
}

/*********************************************************************/

bool IsVarList(const char *var)
{
    if ('@' != var[0])
    {
        return false;
    }
    /*
     * Minimum size for a list is 4:
     * '@' + '(' + name + ')'
     */
    if (strlen(var) < 4)
    {
        return false;
    }
    return true;
}

/*********************************************************************/

static void SetAnyMissingDefaults(EvalContext *ctx, Promise *pp)
/* Some defaults have to be set here, if they involve body-name
   constraints as names need to be expanded before CopyDeRefPromise */
{
    if (strcmp(pp->parent_promise_type->name, "packages") == 0)
    {
        if (PromiseGetConstraint(ctx, pp, "package_method") == NULL)
        {
            PromiseAppendConstraint(pp, "package_method", (Rval) {"generic", RVAL_TYPE_SCALAR}, "any", true);
        }
    }
}

/*********************************************************************/
/* General                                                           */
/*********************************************************************/

typedef struct
{
    bool should_converge;
    bool ok_redefine;
    bool drop_undefined;
    Constraint *cp_save; // e.g. string => "foo"
} ConvergeVariableOptions;

/**
 * @brief Collects variable constraints controlling how the promise should be converged
 */
static ConvergeVariableOptions CollectConvergeVariableOptions(EvalContext *ctx, const Promise *pp, bool allow_redefine)
{
    ConvergeVariableOptions opts = { 0 };
    opts.should_converge = false;
    opts.drop_undefined = false;
    opts.ok_redefine = allow_redefine;
    opts.cp_save = NULL;

    if (pp->done)
    {
        return opts;
    }

    if (!IsDefinedClass(ctx, pp->classes, PromiseGetNamespace(pp)))
    {
        return opts;
    }

    int num_values = 0;
    for (size_t i = 0; i < SeqLength(pp->conlist); i++)
    {
        Constraint *cp = SeqAt(pp->conlist, i);

        if (strcmp(cp->lval, "comment") == 0)
        {
            continue;
        }

        if (cp->rval.item == NULL)
        {
            continue;
        }

        if (strcmp(cp->lval, "ifvarclass") == 0)
        {
            Rval res;

            switch (cp->rval.type)
            {
            case RVAL_TYPE_SCALAR:

                if (!IsDefinedClass(ctx, cp->rval.item, PromiseGetNamespace(pp)))
                {
                    return opts;
                }

                break;

            case RVAL_TYPE_FNCALL:
                {
                    bool excluded = false;

                    /* eval it: e.g. ifvarclass => not("a_class") */

                    res = FnCallEvaluate(ctx, cp->rval.item, NULL).rval;

                    /* Don't continue unless function was evaluated properly */
                    if (res.type != RVAL_TYPE_SCALAR)
                    {
                        RvalDestroy(res);
                        return opts;
                    }

                    excluded = !IsDefinedClass(ctx, res.item, PromiseGetNamespace(pp));

                    RvalDestroy(res);

                    if (excluded)
                    {
                        return opts;
                    }
                }
                break;

            default:
                CfOut(OUTPUT_LEVEL_ERROR, "", "!! Invalid ifvarclass type '%c': should be string or function", cp->rval.type);
                continue;
            }

            continue;
        }

        if (strcmp(cp->lval, "policy") == 0)
        {
            if (strcmp(cp->rval.item, "ifdefined") == 0)
            {
                opts.drop_undefined = true;
                opts.ok_redefine = false;
            }
            else if (strcmp(cp->rval.item, "constant") == 0)
            {
                opts.ok_redefine = false;
            }
            else
            {
                opts.ok_redefine |= true;
            }
        }
        else if (IsDataType(cp->lval))
        {
            num_values++;
            opts.cp_save = cp;
        }
    }

    if (opts.cp_save == NULL)
    {
        CfOut(OUTPUT_LEVEL_INFORM, "", "Warning: Variable body for \"%s\" seems incomplete", pp->promiser);
        PromiseRef(OUTPUT_LEVEL_INFORM, pp);
        return opts;
    }

    if (num_values > 2)
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Variable \"%s\" breaks its own promise with multiple values (code %d)", pp->promiser, num_values);
        PromiseRef(OUTPUT_LEVEL_ERROR, pp);
        return opts;
    }

    opts.should_converge = true;
    return opts;
}

void ConvergeVarHashPromise(EvalContext *ctx, const Promise *pp, bool allow_duplicates)
{
    ConvergeVariableOptions opts = CollectConvergeVariableOptions(ctx, pp, allow_duplicates);
    if (!opts.should_converge)
    {
        return;
    }

    char *scope = NULL;
    if (strcmp("meta", pp->parent_promise_type->name) == 0)
    {
        scope = StringConcatenate(2, PromiseGetBundle(pp)->name, "_meta");
    }
    else
    {
        scope = xstrdup(PromiseGetBundle(pp)->name);
    }

    //More consideration needs to be given to using these
    //a.transaction = GetTransactionConstraints(pp);
    Attributes a = { {0} };
    a.classes = GetClassDefinitionConstraints(ctx, pp);

    Rval existing_var_rval;
    DataType existing_var_type = ScopeGetVariable((VarRef) { NULL, scope, pp->promiser }, &existing_var_rval);
    Buffer *qualified_scope = BufferNew();
    int result = 0;
    if (strcmp(PromiseGetNamespace(pp), "default") == 0)
    {
        result = BufferSet(qualified_scope, scope, strlen(scope));
        if (result < 0)
        {
            /*
             * Even though there will be no problems with memory allocation, there
             * might be other problems.
             */
            UnexpectedError("Problems writing to buffer");
            free(scope);
            BufferDestroy(&qualified_scope);
            return;
        }
    }
    else
    {
        if (strchr(scope, ':') == NULL)
        {
            result = BufferPrintf(qualified_scope, "%s:%s", PromiseGetNamespace(pp), scope);
            if (result < 0)
            {
                /*
                 * Even though there will be no problems with memory allocation, there
                 * might be other problems.
                 */
                UnexpectedError("Problems writing to buffer");
                free(scope);
                BufferDestroy(&qualified_scope);
                return;
            }
        }
        else
        {
            result = BufferSet(qualified_scope, scope, strlen(scope));
            if (result < 0)
            {
                /*
                 * Even though there will be no problems with memory allocation, there
                 * might be other problems.
                 */
                UnexpectedError("Problems writing to buffer");
                free(scope);
                BufferDestroy(&qualified_scope);
                return;
            }
        }
    }

    Rval rval = opts.cp_save->rval;

    if (rval.item != NULL)
    {
        FnCall *fp = (FnCall *) rval.item;

        if (opts.cp_save->rval.type == RVAL_TYPE_FNCALL)
        {
            if (existing_var_type != DATA_TYPE_NONE)
            {
                // Already did this
                free(scope);
                BufferDestroy(&qualified_scope);
                return;
            }

            FnCallResult res = FnCallEvaluate(ctx, fp, pp);

            if (res.status == FNCALL_FAILURE)
            {
                /* We do not assign variables to failed fn calls */
                RvalDestroy(res.rval);
                free(scope);
                BufferDestroy(&qualified_scope);
                return;
            }
            else
            {
                rval = res.rval;
            }
        }
        else
        {
            Buffer *conv = BufferNew();

            if (strcmp(opts.cp_save->lval, "int") == 0)
            {
                result = BufferPrintf(conv, "%ld", IntFromString(opts.cp_save->rval.item));
                if (result < 0)
                {
                    /*
                     * Even though there will be no problems with memory allocation, there
                     * might be other problems.
                     */
                    UnexpectedError("Problems writing to buffer");
                    free(scope);
                    BufferDestroy(&qualified_scope);
                    BufferDestroy(&conv);
                    return;
                }
                rval = RvalCopy((Rval) {(char *)BufferData(conv), opts.cp_save->rval.type});
            }
            else if (strcmp(opts.cp_save->lval, "real") == 0)
            {
                double real_value = 0.0;
                if (DoubleFromString(opts.cp_save->rval.item, &real_value))
                {
                    result = BufferPrintf(conv, "%lf", real_value);
                }
                else
                {
                    result = BufferPrintf(conv, "(double conversion error)");
                }

                if (result < 0)
                {
                    /*
                     * Even though there will be no problems with memory allocation, there
                     * might be other problems.
                     */
                    UnexpectedError("Problems writing to buffer");
                    free(scope);
                    BufferDestroy(&conv);
                    BufferDestroy(&qualified_scope);
                    return;
                }
                rval = RvalCopy((Rval) {(char *)BufferData(conv), opts.cp_save->rval.type});
            }
            else
            {
                rval = RvalCopy(opts.cp_save->rval);
            }
            BufferDestroy(&conv);
        }

        if (Epimenides(PromiseGetBundle(pp)->name, pp->promiser, rval, 0))
        {
            CfOut(OUTPUT_LEVEL_ERROR, "", "Variable \"%s\" contains itself indirectly - an unkeepable promise", pp->promiser);
            exit(1);
        }
        else
        {
            /* See if the variable needs recursively expanding again */

            Rval returnval = EvaluateFinalRval(ctx, BufferData(qualified_scope), rval, true, pp);

            RvalDestroy(rval);

            // freed before function exit
            rval = returnval;
        }

        if (existing_var_type != DATA_TYPE_NONE)
        {
            if (opts.ok_redefine)    /* only on second iteration, else we ignore broken promises */
            {
                ScopeDeleteVariable(BufferData(qualified_scope), pp->promiser);
            }
            else if ((THIS_AGENT_TYPE == AGENT_TYPE_COMMON) && (CompareRval(existing_var_rval, rval) == false))
            {
                switch (rval.type)
                {
                case RVAL_TYPE_SCALAR:
                    CfOut(OUTPUT_LEVEL_VERBOSE, "", " !! Redefinition of a constant scalar \"%s\" (was %s now %s)",
                          pp->promiser, RvalScalarValue(existing_var_rval), RvalScalarValue(rval));
                    PromiseRef(OUTPUT_LEVEL_VERBOSE, pp);
                    break;

                case RVAL_TYPE_LIST:
                    {
                        CfOut(OUTPUT_LEVEL_VERBOSE, "", " !! Redefinition of a constant list \"%s\".", pp->promiser);
                        Writer *w = StringWriter();
                        RlistWrite(w, existing_var_rval.item);
                        char *oldstr = StringWriterClose(w);
                        CfOut(OUTPUT_LEVEL_VERBOSE, "", "Old value: %s", oldstr);
                        free(oldstr);

                        w = StringWriter();
                        RlistWrite(w, rval.item);
                        char *newstr = StringWriterClose(w);
                        CfOut(OUTPUT_LEVEL_VERBOSE, "", " New value: %s", newstr);
                        free(newstr);
                        PromiseRef(OUTPUT_LEVEL_VERBOSE, pp);
                    }
                    break;

                default:
                    break;
                }
            }
        }

        if (IsCf3VarString(pp->promiser))
        {
            // Unexpanded variables, we don't do anything with
            RvalDestroy(rval);
            free(scope);
            BufferDestroy(&qualified_scope);
            return;
        }

        if (!FullTextMatch("[a-zA-Z0-9_\200-\377.]+(\\[.+\\])*", pp->promiser))
        {
            CfOut(OUTPUT_LEVEL_ERROR, "", " !! Variable identifier contains illegal characters");
            PromiseRef(OUTPUT_LEVEL_ERROR, pp);
            RvalDestroy(rval);
            free(scope);
            BufferDestroy(&qualified_scope);
            return;
        }

        if (opts.drop_undefined && rval.type == RVAL_TYPE_LIST)
        {
            for (Rlist *rp = rval.item; rp != NULL; rp = rp->next)
            {
                if (IsNakedVar(rp->item, '@'))
                {
                    free(rp->item);
                    rp->item = xstrdup(CF_NULL_VALUE);
                }
            }
        }

        if (!EvalContextVariablePut(ctx, (VarRef) { NULL, BufferData(qualified_scope), pp->promiser }, rval, DataTypeFromString(opts.cp_save->lval)))
        {
            CfOut(OUTPUT_LEVEL_VERBOSE, "", "Unable to converge %s.%s value (possibly empty or infinite regression)\n", BufferData(qualified_scope), pp->promiser);
            PromiseRef(OUTPUT_LEVEL_VERBOSE, pp);
            cfPS(ctx, OUTPUT_LEVEL_NONE, PROMISE_RESULT_FAIL, "", pp, a, " !! Couldn't add variable %s", pp->promiser);
        }
        else
        {
            cfPS(ctx, OUTPUT_LEVEL_NONE, PROMISE_RESULT_CHANGE, "", pp, a, " -> Added variable %s", pp->promiser);
        }
    }
    else
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", " !! Variable %s has no promised value\n", pp->promiser);
        CfOut(OUTPUT_LEVEL_ERROR, "", " !! Rule from %s at/before line %zu\n", PromiseGetBundle(pp)->source_path, opts.cp_save->offset.line);
        cfPS(ctx, OUTPUT_LEVEL_NONE, PROMISE_RESULT_FAIL, "", pp, a, " !! Couldn't add variable %s", pp->promiser);
    }
    free(scope);
    BufferDestroy(&qualified_scope);
    RvalDestroy(rval);
}

/*********************************************************************/
/* Levels                                                            */
/*********************************************************************/

static bool Epimenides(const char *scope, const char *var, Rval rval, int level)
{
    Rlist *rp, *list;
    char exp[CF_EXPANDSIZE];

    switch (rval.type)
    {
    case RVAL_TYPE_SCALAR:

        if (StringContainsVar(rval.item, var))
        {
            CfOut(OUTPUT_LEVEL_ERROR, "", "Scalar variable \"%s\" contains itself (non-convergent): %s", var, (char *) rval.item);
            return true;
        }

        if (IsCf3VarString(rval.item))
        {
            ExpandPrivateScalar(scope, rval.item, exp);
            CfDebug("bling %d-%s: (look for %s) in \"%s\" => %s \n", level, scope, var, (const char *) rval.item,
                    exp);

            if (level > 3)
            {
                return false;
            }

            if (Epimenides(scope, var, (Rval) {exp, RVAL_TYPE_SCALAR}, level + 1))
            {
                return true;
            }
        }

        break;

    case RVAL_TYPE_LIST:
        list = (Rlist *) rval.item;

        for (rp = list; rp != NULL; rp = rp->next)
        {
            if (Epimenides(scope, var, (Rval) {rp->item, rp->type}, level))
            {
                return true;
            }
        }
        break;

    default:
        return false;
    }

    return false;
}

/*******************************************************************/

static int CompareRval(Rval rval1, Rval rval2)
{
    if (rval1.type != rval2.type)
    {
        return -1;
    }

    switch (rval1.type)
    {
    case RVAL_TYPE_SCALAR:

        if (IsCf3VarString((char *) rval1.item) || IsCf3VarString((char *) rval2.item))
        {
            return -1;          // inconclusive
        }

        if (strcmp(rval1.item, rval2.item) != 0)
        {
            return false;
        }

        break;

    case RVAL_TYPE_LIST:
        return CompareRlist(rval1.item, rval2.item);

    case RVAL_TYPE_FNCALL:
        return -1;

    default:
        return -1;
    }

    return true;
}

/*******************************************************************/

// FIX: this function is a mixture of Equal/Compare (boolean/diff).
// somebody is bound to misuse this at some point
static int CompareRlist(Rlist *list1, Rlist *list2)
{
    Rlist *rp1, *rp2;

    for (rp1 = list1, rp2 = list2; rp1 != NULL && rp2 != NULL; rp1 = rp1->next, rp2 = rp2->next)
    {
        if (rp1->item && rp2->item)
        {
            Rlist *rc1, *rc2;

            if (rp1->type == RVAL_TYPE_FNCALL || rp2->type == RVAL_TYPE_FNCALL)
            {
                return -1;      // inconclusive
            }

            rc1 = rp1;
            rc2 = rp2;

            // Check for list nesting with { fncall(), "x" ... }

            if (rp1->type == RVAL_TYPE_LIST)
            {
                rc1 = rp1->item;
            }

            if (rp2->type == RVAL_TYPE_LIST)
            {
                rc2 = rp2->item;
            }

            if (IsCf3VarString(rc1->item) || IsCf3VarString(rp2->item))
            {
                return -1;      // inconclusive
            }

            if (strcmp(rc1->item, rc2->item) != 0)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }

    return true;
}

/*******************************************************************/

static void CheckRecursion(EvalContext *ctx, const ReportContext *report_context, Promise *pp)
{
    char *type;
    char *scope;
    Bundle *bp;
    FnCall *fp;

    // Check for recursion of bundles so that knowledge map will reflect these cases

    if (strcmp("services", pp->parent_promise_type->name) == 0)
    {
        ParseServices(ctx, report_context, pp);
    }

    for (size_t i = 0; i < SeqLength(pp->conlist); i++)
    {
        Constraint *cp = SeqAt(pp->conlist, i);

        if (strcmp("usebundle", cp->lval) == 0)
        {
            type = "agent";
        }
        else  if (strcmp("edit_line", cp->lval) == 0 || strcmp("edit_xml", cp->lval) == 0)
        {
            type = cp->lval;
        }
        else
        {
            continue;
        }

        switch (cp->rval.type)
        {
           case RVAL_TYPE_SCALAR:
               scope = (char *)cp->rval.item;
               break;

           case RVAL_TYPE_FNCALL:
               fp = (FnCall *)cp->rval.item;
               scope = fp->name;
               break;

           default:
               continue;
        }

        {
            Policy *policy = PolicyFromPromise(pp);
            bp = PolicyGetBundle(policy, NULL, type, scope);
            if (!bp)
            {
                bp = PolicyGetBundle(policy, NULL, "common", scope);
            }
        }

        if (bp)
        {
            EvalContextStackPushBundleFrame(ctx, bp, false);
            for (size_t j = 0; j < SeqLength(bp->promise_types); j++)
            {
                PromiseType *sbp = SeqAt(bp->promise_types, j);

                for (size_t ppsubi = 0; ppsubi < SeqLength(sbp->promises); ppsubi++)
                {
                    Promise *ppsub = SeqAt(sbp->promises, ppsubi);
                    ExpandPromise(ctx, AGENT_TYPE_COMMON, ppsub, NULL, report_context);
                }
            }
            EvalContextStackPopFrame(ctx);
        }
    }
}

/*****************************************************************************/

static void ParseServices(EvalContext *ctx, const ReportContext *report_context, Promise *pp)
{
    FnCall *default_bundle = NULL;
    Rlist *args = NULL;
    Attributes a = { {0} };

    a = GetServicesAttributes(ctx, pp);

    // Need to set up the default service pack to eliminate syntax, analogous to verify_services.c

    if (ConstraintGetRvalValue(ctx, "service_bundle", pp, RVAL_TYPE_SCALAR) == NULL)
    {
        switch (a.service.service_policy)
        {
        case SERVICE_POLICY_START:
            RlistAppendScalar(&args, pp->promiser);
            RlistAppendScalar(&args, "start");
            break;

        case SERVICE_POLICY_RESTART:
            RlistAppendScalar(&args, pp->promiser);
            RlistAppendScalar(&args, "restart");
            break;

        case SERVICE_POLICY_RELOAD:
            RlistAppendScalar(&args, pp->promiser);
            RlistAppendScalar(&args, "restart");
            break;

        case SERVICE_POLICY_STOP:
        case SERVICE_POLICY_DISABLE:
        default:
            RlistAppendScalar(&args, pp->promiser);
            RlistAppendScalar(&args, "stop");
            break;

        }

        default_bundle = FnCallNew("standard_services", args);

        PromiseAppendConstraint(pp, "service_bundle", (Rval) {default_bundle, RVAL_TYPE_FNCALL}, "any", false);
        a.havebundle = true;
    }

// Set $(this.service_policy) for flexible bundle adaptation

    switch (a.service.service_policy)
    {
    case SERVICE_POLICY_START:
        ScopeNewSpecialScalar(ctx, "this", "service_policy", "start", DATA_TYPE_STRING);
        break;

    case SERVICE_POLICY_RESTART:
        ScopeNewSpecialScalar(ctx, "this", "service_policy", "restart", DATA_TYPE_STRING);
        break;

    case SERVICE_POLICY_RELOAD:
        ScopeNewSpecialScalar(ctx, "this", "service_policy", "reload", DATA_TYPE_STRING);
        break;
        
    case SERVICE_POLICY_STOP:
    case SERVICE_POLICY_DISABLE:
    default:
        ScopeNewSpecialScalar(ctx, "this", "service_policy", "stop", DATA_TYPE_STRING);
        break;
    }

    Bundle *bp = PolicyGetBundle(PolicyFromPromise(pp), NULL, "agent", default_bundle->name);
    if (!bp)
    {
        bp = PolicyGetBundle(PolicyFromPromise(pp), NULL, "common", default_bundle->name);
    }

    if (default_bundle && bp == NULL)
    {
        return;
    }

    if (bp)
    {
        EvalContextStackPushBundleFrame(ctx, bp, false);
        ScopeMapBodyArgs(ctx, bp->name, args, bp->args);

        for (size_t i = 0; i < SeqLength(bp->promise_types); i++)
        {
            PromiseType *sbp = SeqAt(bp->promise_types, i);

            for (size_t ppsubi = 0; ppsubi < SeqLength(sbp->promises); ppsubi++)
            {
                Promise *ppsub = SeqAt(sbp->promises, ppsubi);
                ExpandPromise(ctx, AGENT_TYPE_COMMON, ppsub, NULL, report_context);
            }
        }

        EvalContextStackPopFrame(ctx);
    }
}
