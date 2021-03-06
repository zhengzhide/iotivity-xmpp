# -*- coding: utf-8 -*-

# *********************************************************************
#
# Copyright 2014 Intel Mobile Communications GmbH All Rights Reserved.
#
# *********************************************************************
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http:#www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# *********************************************************************

# This builder executes configure

import os, subprocess
import SCons.Builder, SCons.Node, SCons.Errors

# Create the output message
#
# @param s original message
# @param target target name
# @param source source name
# @param env environment object
def __message( s, target, source, env ) :
    print "Configuring using [%s] ..." % (source[0])

# Create the action
#
# @param target target file on the local drive
# @param source URL for download
# @@param env environment object
def __action( target, source, env ) :
    cmd = None

    # Windows...
    if env["PLATFORM"] in ["win32"] :
        if env.WhereIs("cmd") :
            # TODO: Add Windows Support
            cmd = None

    # read the tools on *nix systems and sets the default parameters
    elif env["PLATFORM"] in ["darwin", "linux", "posix"] :
        if env.WhereIs("sh") :
            cmd = "./configure"

    if not cmd :
        raise SCons.Errors.StopError("Configure shell on this platform [%s] unknown" % (env["PLATFORM"]))

    # We need to be in the target's directory
    cwd = os.path.dirname(os.path.realpath(target[0].path))

    # build it now (we need the shell, because some programs need it)
    devnull = open(os.devnull, "wb")
    handle  = subprocess.Popen( cmd, shell=True, cwd=cwd, stdout=devnull )

    if handle.wait() <> 0 :
        raise SCons.Errors.BuildError( "Configuring script [%s] on the source [%s]" % (cmd, source[0])  )

# Define the builder's emitter
#
# @param target target file on the local drive
# @param source 
# @param env environment object
def __emitter( target, source, env ) :
    return target, source

# Generate function which adds the builder to the environment,
#
# @param env environment object
def generate( env ) :
    env["BUILDERS"]["Configure"] = SCons.Builder.Builder( action = __action,  emitter = __emitter,  target_factory = SCons.Node.FS.File,  source_factory = SCons.Node.FS.File,  single_source = True,  PRINT_CMD_LINE_FUNC = __message )

# Exist function for the builder
#
# @param env environment object
# @return true
def exists( env ) :
    return 1
