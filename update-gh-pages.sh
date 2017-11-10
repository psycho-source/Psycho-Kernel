#!/bin/bash

VER="-v7.0"
CODE="Kernel"
BASEVER="Psycho-"
if [ "$TRAVIS_PULL_REQUEST" == "false" ]; then
  echo -e "Starting to update gh-pages\n"

  #copy data we're interested in to other place
  mkdir $HOME/Builds
  cd $HOME/kernel/flashable
  zip -r `echo $BASEVER$CODE$VER`.zip *
  mv `echo $BASEVER$CODE$VER`.zip $HOME/Builds
  cd $HOME/Builds
  md5sum `echo $BASEVER$CODE$VER`.zip > md5.txt
  sha256sum `echo $BASEVER$CODE$VER`.zip > sha256.txt

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
  git commit -m "Travis build $TRAVIS_BUILD_NUMBER pushed to gh-pages"
  git push -fq origin gh-pages > /dev/null

  echo -e "Done\n"
fi
