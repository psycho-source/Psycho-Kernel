#!/bin/bash

BASEVER="Psycho-Kernel"
VER="-v3"
#Update VER Variable after every update
if [ "$TRAVIS_PULL_REQUEST" == "false" ]; then
  echo -e "Starting to update gh-pages\n"

  #copy data we're interested in to other place
  mkdir $HOME/Builds
  cd $HOME/kernel/flashable
  zip -r `echo $BASEVER$VER`.zip *
  mv `echo $BASEVER$VER`.zip $HOME/Builds

  #go to home and setup git
  cd $HOME
  git config --global user.email "travis@travis-ci.org"
  git config --global user.name "Travis"

  #using token clone gh-pages branch
  git clone --quiet --branch=gh-pages https://${GH_TOKEN}@github.com/psycho-source/Psycho-Kernel.git  gh-pages > /dev/null

  #go into diractory and copy data we're interested in to that directory
  cp -Rf $HOME/Builds/* $HOME/gh-pages/Stable/

  #add, commit and push files
  cd gh-pages
  git add -f .
  git commit -m "New Stable Build $BASEVER$VER pushed to gh-pages/Stable"
  git push -fq origin gh-pages > /dev/null

  echo -e "Done\n"
fi